// vfwutil.cxx

#include <stdio.h>
#include <windows.h>
#include <shlobj.h>

#include "vfwutil.h"
#include "resource.h"

int CALLBACK BrowseCallbackProc(HWND hwnd, UINT uMsg, LPARAM /*lParam*/, LPARAM lpData)
{
  if(uMsg==BFFM_INITIALIZED){
    SendMessage(hwnd, BFFM_SETSELECTION, (WPARAM)TRUE, lpData);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE|SWP_NOSIZE);
  }
  return 0;
}
bool getFolderName(char *foldername, HWND phwnd)
{
  bool ret=false;
  LPMALLOC pMalloc;
  SHGetMalloc(&pMalloc);
  
  BROWSEINFO bi;
  bi.hwndOwner=phwnd;
  bi.pidlRoot=NULL;
  bi.pszDisplayName=foldername;
  bi.lpszTitle=("Select AVIdata-folder.");
  bi.ulFlags=BIF_RETURNONLYFSDIRS;
  bi.lpfn=BrowseCallbackProc;
  bi.lParam=(LPARAM)foldername;
  bi.iImage=0;
  
  ITEMIDLIST *idl=SHBrowseForFolder(&bi);
  if(idl!=NULL){
    if(SHGetPathFromIDList(idl, foldername)!=FALSE){
      ret=true;
    }
    pMalloc->Free(idl);
  }
  pMalloc->Release();
  return ret;
}

enum WaveInCallback{
  wavein_callback_message=1,
  wavein_callback_event=2,
};

#define VID_UNCOMPRESSED_CODEC 0x20424944    // 'D''I''B'' '
#define VID_CAPTURE_FPS 15                   // 15FPS
#define VID_CAPTURE_BITSPERSAMPLE 32         // 32bit/pixel

#define AUD_CAPTURE_SAMPLINGFREQUENCY 32000  // 32000Hz
#define AUD_CAPTURE_CHANNELS 2               // stereo
#define AUD_CAPTURE_BITSPERSAMPLE 16         // 16bit/sample

#define MP3_BITRATE (128*1000)               // 128khz CBR
#define MP3_SAMPLESPERFRAME 1152             // mp3 v1 layerIII
#define MP3_FRAMESIZE (MP3_SAMPLESPERFRAME*(MP3_BITRATE/8)/AUD_CAPTURE_SAMPLINGFREQUENCY)

#define WAV_BUFSIZE MP3_SAMPLESPERFRAME*4*16
#define MP3_BUFSIZE WAV_BUFSIZE

#define WAV_BUFCOUNT 3


typedef struct _THREADINFO
{
  volatile HANDLE hthread;
  volatile bool enableflg;
  
  unsigned int timeout; // seconds
  
  DWORD aud_codec, aud_khz;
  DWORD vid_codec, vid_fps, vid_duration;

  WORD aud_bitspersample; // 8/16/24
  WORD aud_channels;      // 1/2/..
  WORD vid_bitspersample; // 24/32

  RECT  img_rect;
  DWORD aud_bitrate;

  // acm
  HACMDRIVER had;
  HACMSTREAM has;

  // target_window
  HWND tar_hwnd;
  BITMAPINFOHEADER tar_bi;
  int tar_x, tar_y, tar_w, tar_h, tar_bitcount;
  DWORD dwrop;

  // report_window
  HWND rep_hwnd;
  DWORD rep_mes;

  // AVI/VFW
  PAVIFILE avifile;
  PAVISTREAM pvid, paud;

  DWORD aud_samplecount;
  DWORD vid_samplecount;
  DWORD vid_dropframecount;

  // video buffer
  unsigned char *vidbuf;
  
  // audio buffer
  char wavbuf[WAV_BUFCOUNT][WAV_BUFSIZE];
  unsigned char mp3buf[MP3_BUFSIZE];

  // MME
  HWAVEIN hwi;
  WAVEHDR whin[WAV_BUFCOUNT];
  HANDLE mmcapevent;
  
  //DirectSound
  LPDIRECTSOUNDCAPTURE8 dscapdev;
  LPDIRECTSOUNDCAPTUREBUFFER dsbuf;
  HANDLE dscapevent[WAV_BUFCOUNT];

}THREADINFO, *LPTHREADINFO;

static THREADINFO ti={0};

DWORD wav2mp3(HACMSTREAM has,
              unsigned char *mp3buf, DWORD mp3bufsize, unsigned char *wavbuf, DWORD wavbufsize)
{
  ACMSTREAMHEADER ash;
  memset(&ash, 0, sizeof(ACMSTREAMHEADER));
  ash.cbStruct=sizeof(ACMSTREAMHEADER);

  ash.pbSrc=wavbuf;
  ash.cbSrcLength=wavbufsize;
  ash.pbDst=mp3buf;
  ash.cbDstLength=mp3bufsize;
  
  acmStreamPrepareHeader(has, &ash, 0);
  acmStreamConvert(has, &ash, ACM_STREAMCONVERTF_BLOCKALIGN);
  acmStreamUnprepareHeader(has, &ash, 0);

  return ash.cbDstLengthUsed;
}

void MyAVIRec_writeaudio(WAVEHDR *whdone)
{
  // wavein_message
  if(whdone->dwFlags&WHDR_DONE){

    char *wavbuf=whdone->lpData;
    DWORD wavbufsize=whdone->dwBytesRecorded;

    if(ti.has==NULL){
      AVIStreamWrite(ti.paud,
                     ti.aud_samplecount++, 1, wavbuf, wavbufsize,
                     AVIIF_KEYFRAME, NULL, NULL);
    }else{
      DWORD mp3bufsize=wav2mp3(ti.has, ti.mp3buf, MP3_BUFSIZE,
                               (unsigned char *)wavbuf, wavbufsize);
      AVIStreamWrite(ti.paud,
                     ti.aud_samplecount++, 1, ti.mp3buf, mp3bufsize,
                     AVIIF_KEYFRAME, NULL, NULL);
    }

    waveInUnprepareHeader(ti.hwi, whdone, sizeof(WAVEHDR));
    if(ti.enableflg==true){
      waveInPrepareHeader(ti.hwi, whdone, sizeof(WAVEHDR));
      waveInAddBuffer(ti.hwi, whdone, sizeof(WAVEHDR));
    }
  }
}

DWORD WINAPI thrProc_vfwrec(LPVOID /*lpparam*/)
{
  DWORD tid=GetCurrentThreadId();
  DWORD tid_tar=GetWindowThreadProcessId(ti.tar_hwnd, NULL);
  POINT clientofs={0,0};
  ClientToScreen(ti.tar_hwnd, &clientofs);

  HDC hdcmem=CreateCompatibleDC(NULL);
  HBITMAP hbmp=CreateDIBSection(NULL, (LPBITMAPINFO)&ti.tar_bi, DIB_RGB_COLORS,
                                (void **)(&ti.vidbuf), NULL, 0);
  HBITMAP hbmpprev=(HBITMAP)SelectObject(hdcmem, hbmp);
  HDC hdc=GetDC(ti.tar_hwnd);

  if(ti.dsbuf!=NULL){
    // directsound
    ti.dsbuf->Start(DSCBSTART_LOOPING);
  }else{
    // mme
    for(int i=0; i<WAV_BUFCOUNT; i++){
      memset(ti.whin+i, 0, sizeof(WAVEHDR));
      ti.whin[i].lpData=ti.wavbuf[i];
      ti.whin[i].dwBufferLength=WAV_BUFSIZE;
      ti.whin[i].dwUser=(DWORD_PTR)i;
      waveInPrepareHeader(ti.hwi, ti.whin+i, sizeof(WAVEHDR));
      waveInAddBuffer(ti.hwi, ti.whin+i, sizeof(WAVEHDR));
    }
    waveInStart(ti.hwi);
  }
  
  DWORD starttime=timeGetTime();
  while(timeGetTime()-starttime<ti.timeout*1000){
    DWORD nexttime=timeGetTime()+ti.vid_duration;

    // audio
    if(ti.dsbuf==NULL){
      if(ti.mmcapevent!=NULL){
        // mme+wavein_event
        DWORD ret=WaitForSingleObject(ti.mmcapevent, 0);
        if((ret!=WAIT_FAILED)&&(ret!=WAIT_TIMEOUT)){
          for(int i=0; i<WAV_BUFCOUNT; i++){
            if(ti.whin[i].dwFlags&WHDR_DONE){
              char *wavbuf=ti.whin[i].lpData;
              DWORD wavbufsize=ti.whin[i].dwBytesRecorded;
              if(ti.has==NULL){
                AVIStreamWrite(ti.paud,
                               ti.aud_samplecount++, 1, wavbuf, wavbufsize,
                               AVIIF_KEYFRAME, NULL, NULL);
              }else{
                DWORD mp3bufsize=wav2mp3(ti.has, ti.mp3buf, MP3_BUFSIZE,
                                         (unsigned char *)wavbuf, wavbufsize);
                AVIStreamWrite(ti.paud,
                               ti.aud_samplecount++, 1, ti.mp3buf, mp3bufsize,
                               AVIIF_KEYFRAME, NULL, NULL);
              }
              waveInUnprepareHeader(ti.hwi, ti.whin+i, sizeof(WAVEHDR));
              if(ti.enableflg==true){
                waveInPrepareHeader(ti.hwi, ti.whin+i, sizeof(WAVEHDR));
                waveInAddBuffer(ti.hwi, ti.whin+i, sizeof(WAVEHDR));
              }
            }
          }
          if(ti.enableflg==false) break;
        }

      }else{
        if(ti.enableflg==false) break;
      }
    }else if(ti.dsbuf!=NULL){
      // directsound
      DWORD ret=WaitForMultipleObjects(WAV_BUFCOUNT, ti.dscapevent, FALSE, 0);

      if((ret!=WAIT_FAILED)&&(ret!=WAIT_TIMEOUT)){
        DWORD index=ret-WAIT_OBJECT_0;

        void *wavbuf=NULL, *wavbuf_ex=NULL;
        DWORD wavbufsize=0, wavbufsize_ex=0;
        ti.dsbuf->Lock(index*WAV_BUFSIZE, WAV_BUFSIZE,
                       &wavbuf, &wavbufsize, &wavbuf_ex, &wavbufsize_ex, 0);

        if(ti.has==NULL){
          AVIStreamWrite(ti.paud,
                         ti.aud_samplecount++, 1, wavbuf, wavbufsize,
                         AVIIF_KEYFRAME, NULL, NULL);
        }else{
          DWORD mp3bufsize=wav2mp3(ti.has, ti.mp3buf, MP3_BUFSIZE,
                                   (unsigned char *)wavbuf, WAV_BUFSIZE);
          AVIStreamWrite(ti.paud,
                         ti.aud_samplecount++, 1, ti.mp3buf, mp3bufsize,
                         AVIIF_KEYFRAME, NULL, NULL);
        }
        ti.dsbuf->Unlock(wavbuf, wavbufsize, wavbuf_ex, wavbufsize_ex);

        if(ti.enableflg==false) break;
      }
    }

  // video
    BitBlt(hdcmem, 0, 0, ti.tar_bi.biWidth, ti.tar_bi.biHeight,
           hdc, ti.tar_x, ti.tar_y, ti.dwrop);

    if(ti.dwrop&CAPTUREBLT){
      if(tid!=tid_tar) AttachThreadInput(tid_tar, tid, TRUE);
      CURSORINFO ci;
      ci.cbSize=sizeof(CURSORINFO);
      GetCursorInfo(&ci);
      if(ci.flags&CURSOR_SHOWING){
        ICONINFO ii;
        GetIconInfo(ci.hCursor, &ii);
        int x=ci.ptScreenPos.x-ii.xHotspot;
        int y=ci.ptScreenPos.y-ii.yHotspot;
        DrawIcon(hdcmem, x-clientofs.x-ti.tar_x, y-clientofs.y-ti.tar_y, ci.hCursor);
      }
      if(tid!=tid_tar) AttachThreadInput(tid_tar, tid, FALSE);
    }

    AVIStreamWrite(ti.pvid, ti.vid_samplecount++, 1,
                   ti.vidbuf, ti.tar_bi.biSizeImage, AVIIF_KEYFRAME, NULL, NULL);

    // sleep
    DWORD currenttime=timeGetTime();
    if(nexttime<currenttime){
      // dropping frame!
      for(;;){
        ti.vid_dropframecount++;
        AVIStreamWrite(ti.pvid, ti.vid_samplecount++, 1,
                       ti.vidbuf, ti.tar_bi.biSizeImage, AVIIF_KEYFRAME, NULL, NULL);
        //             NULL, 0, AVIIF_KEYFRAME, NULL, NULL); is it ok?
        nexttime+=ti.vid_duration;
        currenttime=timeGetTime();
        if(nexttime>currenttime){
          Sleep(nexttime-currenttime);
          break;
        }
      }
    }else{
      Sleep(nexttime-currenttime);
    }
  }
  ti.enableflg=false;

  if(ti.dscapdev==NULL){
    waveInReset(ti.hwi);
    Sleep(1500); // consume wave-buffer
    for(int i=0; i<WAV_BUFCOUNT; i++){
      waveInUnprepareHeader(ti.hwi, ti.whin+i, sizeof(WAVEHDR));
    }
  }else{
    ti.dsbuf->Stop();
  }

  ReleaseDC(ti.tar_hwnd, hdc);
  SelectObject(hdcmem, hbmpprev);
  DeleteObject(hbmp);
  DeleteDC(hdcmem);

  PostMessage(ti.rep_hwnd, ti.rep_mes,
              (WPARAM)ti.vid_samplecount, (LPARAM)ti.vid_dropframecount);

  CloseHandle(ti.hthread);
  ti.hthread=NULL;

  ExitThread(0);
}






void MyAVIRec_free()
{
  if(ti.paud!=NULL) AVIStreamRelease(ti.paud);
  if(ti.pvid!=NULL) AVIStreamRelease(ti.pvid);
  ti.paud=NULL;
  ti.pvid=NULL;

  if(ti.has!=NULL) acmStreamClose(ti.has, 0);
  if(ti.had!=NULL) acmDriverClose(ti.had, 0);
  if(ti.hwi!=NULL) waveInClose(ti.hwi);
  ti.has=NULL;
  ti.had=NULL;
  ti.hwi=NULL;

  if(ti.dsbuf!=NULL) ti.dsbuf->Release();
  if(ti.dscapdev!=NULL) ti.dscapdev->Release();
  ti.dsbuf=NULL;
  ti.dscapdev=NULL;

  if(ti.mmcapevent!=NULL) CloseHandle(ti.mmcapevent);
  ti.mmcapevent=NULL;
  for(int i=0; i<WAV_BUFCOUNT; i++){
    if(ti.dscapevent[i]!=NULL) CloseHandle(ti.dscapevent[i]);
    ti.dscapevent[i]=NULL;
  }

  if(ti.avifile!=NULL) AVIFileRelease(ti.avifile);
  ti.avifile=NULL;
}

bool MyAVIRec_recording()
{
  if(ti.enableflg==false) return false;
  if(ti.hthread==NULL) return false;
  return true;
}

void MyAVIRec_stop()
{
  if(MyAVIRec_recording()==true){
    ti.enableflg=false;
    WaitForSingleObject(ti.hthread, 3000); // wait 3000msec max
  }
}

PAVISTREAM createVIDStream(PAVIFILE pfile)
{
  if(pfile==NULL) return NULL;

  AVISTREAMINFO si;
  memset(&si, 0, sizeof(AVISTREAMINFO));
  si.fccType=streamtypeVIDEO;
  si.dwScale=ti.vid_duration;
  si.dwRate=1000; // 1000/vid_duration=vid_fps
  si.fccHandler=ti.vid_codec;
  si.rcFrame=ti.img_rect;
  
  PAVISTREAM pvid;
  DWORD ret=AVIFileCreateStream(pfile, &pvid, &si);
  if(ret!=0) return NULL;

  if(ti.vid_codec!=VID_UNCOMPRESSED_CODEC){
    AVICOMPRESSOPTIONS op;
    memset(&op, 0, sizeof(AVICOMPRESSOPTIONS));
    op.fccType=streamtypeVIDEO;
    op.fccHandler=ti.vid_codec;

    PAVISTREAM pvidcomp;
    DWORD aviret=AVIMakeCompressedStream(&pvidcomp, pvid, &op, NULL);
    if(aviret!=AVIERR_OK) return pvid;
    AVIStreamRelease(pvid);
    return pvidcomp;
  }else{
    return pvid;
  }
}

bool MyAVIRec_start(char *filename,
                    DWORD waveindeviceindex, GUID *soundcapturedevguid,
                    DWORD vid_fourCC, HACMDRIVERID hmp3driverid,
                    HWND hwnd, DWORD dwrop, int x, int y, int w, int h, int bitspersample,
                    HWND rep_hwnd, DWORD rep_mes, unsigned int timeout)
{
  int waveincallbacktype=wavein_callback_event;
  
  if(ti.avifile!=NULL) return false;
  if(ti.enableflg==true) return false;
  if(ti.hthread!=NULL) return false;

  if(timeout==0) timeout=60*60*1000; // 60•ª
  
  memset(&ti, 0, sizeof(THREADINFO));
  ti.enableflg=true;

  DeleteFile(filename);
  AVIFileOpen(&ti.avifile, filename, OF_WRITE|OF_CREATE, NULL);
  if(ti.avifile==NULL) return false;

  // settings
  ti.vid_codec=vid_fourCC;
  ti.vid_fps=VID_CAPTURE_FPS;
  ti.vid_bitspersample=(WORD)bitspersample;
  ti.vid_duration=(int)(1000.0/(double)(ti.vid_fps)+0.5);

  if(hmp3driverid==NULL){
    ti.aud_codec=WAVE_FORMAT_PCM;
  }else{
    ti.aud_codec=WAVE_FORMAT_MPEGLAYER3;
  }
  ti.aud_khz=AUD_CAPTURE_SAMPLINGFREQUENCY;
  ti.aud_bitspersample=AUD_CAPTURE_BITSPERSAMPLE;
  ti.aud_channels=AUD_CAPTURE_CHANNELS;

  SetRect(&ti.img_rect, 0, 0, w, h);
  ti.aud_bitrate=MP3_BITRATE;

  BITMAPINFOHEADER bi;
  memset(&bi, 0, sizeof(BITMAPINFOHEADER));
  bi.biSize=sizeof(BITMAPINFOHEADER);
  bi.biWidth=w;
  bi.biHeight=h;
  bi.biPlanes=1;
  bi.biBitCount=ti.vid_bitspersample;
  bi.biCompression=BI_RGB;
  bi.biSizeImage=h*((w*bi.biBitCount+31)/32*4);

  WAVEFORMATEX wfx;
  memset(&wfx, 0, sizeof(WAVEFORMATEX));
  wfx.wFormatTag=WAVE_FORMAT_PCM;
  wfx.nChannels=ti.aud_channels;
  wfx.nSamplesPerSec=ti.aud_khz;
  
  wfx.wBitsPerSample=ti.aud_bitspersample;
  wfx.nBlockAlign=wfx.wBitsPerSample/8*wfx.nChannels;
  wfx.nAvgBytesPerSec=wfx.nSamplesPerSec*wfx.nBlockAlign;
  
  MPEGLAYER3WAVEFORMAT mp3wf;
  memset(&mp3wf, 0, sizeof(MPEGLAYER3WAVEFORMAT));
  mp3wf.wfx.wFormatTag=WAVE_FORMAT_MPEGLAYER3;
  mp3wf.wfx.nChannels=ti.aud_channels;
  mp3wf.wfx.nSamplesPerSec=ti.aud_khz;
  
  mp3wf.wfx.wBitsPerSample=0;
  mp3wf.wfx.nBlockAlign=1;
  mp3wf.wfx.nAvgBytesPerSec=ti.aud_bitrate/8;
  
  mp3wf.wfx.cbSize=MPEGLAYER3_WFX_EXTRA_BYTES;
  mp3wf.wID=MPEGLAYER3_ID_MPEG;
  mp3wf.fdwFlags=MPEGLAYER3_FLAG_PADDING_OFF;
  mp3wf.nFramesPerBlock=1;
  mp3wf.nBlockSize=MP3_FRAMESIZE*mp3wf.nFramesPerBlock;
  mp3wf.nCodecDelay=1393;

  // video stream
  ti.pvid=createVIDStream(ti.avifile);
  if(ti.pvid==NULL){
    AVIFileRelease(ti.avifile);
    return false;
  }
  AVIStreamSetFormat(ti.pvid, 0, &bi, sizeof(BITMAPINFOHEADER));

  // audio
  bool with_audiostream=false;

  // formatconverter
  MMRESULT mmr;
  if(hmp3driverid!=NULL){
    acmDriverOpen(&ti.had, hmp3driverid, 0);
    mmr=acmStreamOpen(&ti.has, ti.had, &wfx, (LPWAVEFORMATEX)&mp3wf, NULL, 0, 0, 0);
    if(mmr!=MMSYSERR_NOERROR){
      ti.aud_codec=WAVE_FORMAT_PCM;
      hmp3driverid=NULL;
    }
  }

  // audio source
  if(*soundcapturedevguid==GUID_NULL){
    // mmr
    if(waveindeviceindex==(DWORD)WAVE_MAPPER-1) goto cannot_create_audiostream;

    if(waveincallbacktype==wavein_callback_message){
      mmr=waveInOpen(&ti.hwi, waveindeviceindex, &wfx,
                     (DWORD_PTR)rep_hwnd, 0, CALLBACK_WINDOW);
    }else if(waveincallbacktype==wavein_callback_event){
      ti.mmcapevent=CreateEvent(NULL, FALSE, FALSE, NULL);
      mmr=waveInOpen(&ti.hwi, waveindeviceindex, &wfx,
                     (DWORD_PTR)ti.mmcapevent, 0, CALLBACK_EVENT);
    }else{
      mmr=MMSYSERR_NOERROR+1;
    }
    if(mmr!=MMSYSERR_NOERROR) goto cannot_create_audiostream;
  }else{
  // directsound
    if(DirectSoundCaptureCreate8(soundcapturedevguid, &ti.dscapdev, NULL)!=S_OK){
      goto cannot_create_audiostream;
    }

    DSCBUFFERDESC desc;
    memset(&desc, 0, sizeof(DSCBUFFERDESC));
    desc.dwSize=sizeof(DSCBUFFERDESC);
    desc.dwFlags=0;
    desc.dwBufferBytes=WAV_BUFSIZE*WAV_BUFCOUNT;
    desc.lpwfxFormat=&wfx;
    if(ti.dscapdev->CreateCaptureBuffer(&desc, &ti.dsbuf, NULL)!=DS_OK){
      goto cannot_create_audiostream;
    }

    DSCBCAPS caps;
    memset(&caps, 0, sizeof(DSCBCAPS));
    caps.dwSize=sizeof(DSCBCAPS);
    ti.dsbuf->GetCaps(&caps);
    if(desc.dwBufferBytes!=caps.dwBufferBytes) goto cannot_create_audiostream;

    LPDIRECTSOUNDNOTIFY8 notify;
    ti.dsbuf->QueryInterface(IID_IDirectSoundNotify, (LPVOID *)&notify);

    DSBPOSITIONNOTIFY dsbpos[WAV_BUFCOUNT];
    for(int i=0; i<WAV_BUFCOUNT; i++){
      dsbpos[i].dwOffset=WAV_BUFSIZE*(i+1)-1;
      dsbpos[i].hEventNotify=ti.dscapevent[i]=CreateEvent(NULL, FALSE, FALSE, NULL);
    }
    notify->SetNotificationPositions(WAV_BUFCOUNT, dsbpos);
    notify->Release();
  }
  
  // audio stream
  AVISTREAMINFO si;
  memset(&si, 0, sizeof(AVISTREAMINFO));
  si.fccType=streamtypeAUDIO;
  if(hmp3driverid==NULL){
    // wav
    si.dwSampleSize=ti.aud_bitspersample/8*ti.aud_channels;
    si.dwScale=1;
    si.dwRate=ti.aud_khz;
    AVIFileCreateStream(ti.avifile, &ti.paud, &si);
    if(ti.paud==NULL) goto cannot_create_audiostream;
    AVIStreamSetFormat(ti.paud, 0, &wfx, sizeof(WAVEFORMATEX));
  }else{
    // mp3
    si.dwSampleSize=MP3_FRAMESIZE;
    si.dwScale=1;
    si.dwRate=MP3_BITRATE/8;
    AVIFileCreateStream(ti.avifile, &ti.paud, &si);
    if(ti.paud==NULL) goto cannot_create_audiostream;
    AVIStreamSetFormat(ti.paud, 0, (LPWAVEFORMATEX)&mp3wf, sizeof(MPEGLAYER3WAVEFORMAT));
  } 
  with_audiostream=true;

 cannot_create_audiostream:
  if(with_audiostream==false){
    if(ti.paud!=NULL) AVIStreamRelease(ti.paud);
    if(ti.has!=NULL) acmStreamClose(ti.has, 0);
    if(ti.had!=NULL) acmDriverClose(ti.had, 0);
    if(ti.hwi!=NULL) waveInClose(ti.hwi);
    ti.paud=NULL;
    ti.has=NULL;
    ti.had=NULL;
    ti.hwi=NULL;
    
    if(ti.dscapdev!=NULL) ti.dscapdev->Release();
    if(ti.dsbuf!=NULL) ti.dsbuf->Release();
    ti.dsbuf=NULL;
    ti.dscapdev=NULL;

    if(ti.mmcapevent!=NULL) CloseHandle(ti.mmcapevent);
    ti.mmcapevent=NULL;
    for(int i=0; i<WAV_BUFCOUNT; i++){
      if(ti.dscapevent[i]!=NULL) CloseHandle(ti.dscapevent[i]);
      ti.dscapevent[i]=NULL;
    }
  }
    
  ti.tar_hwnd=hwnd;
  ti.tar_bi=bi;
  ti.tar_x=x;
  ti.tar_y=y;
  ti.dwrop=dwrop;
  
  ti.rep_hwnd=rep_hwnd;
  ti.rep_mes=rep_mes;
  ti.timeout=timeout;

  ti.aud_samplecount=0;
  ti.vid_samplecount=0;
  ti.vid_dropframecount=0;

  ti.hthread=CreateThread(NULL, 0,
                          (LPTHREAD_START_ROUTINE)thrProc_vfwrec,
                          NULL, CREATE_SUSPENDED, NULL);
  SetThreadPriority(ti.hthread, THREAD_PRIORITY_HIGHEST);
  ResumeThread(ti.hthread);

  return true;
}

VFWRec::~VFWRec()
{
  MyAVIRec_free();
}

bool VFWRec::RecStart()
{
  SendMessage(m_hdlg, WM_COMMAND, IDC_VFW_BUTTON1, 0);
}

BOOL CALLBACK VFWRec::dSoundEnumCB(LPGUID guid, LPCSTR desc, LPCSTR /*module*/, LPVOID user)
{
  VFWRec *vfwrec=(VFWRec *)user;
  
  if((guid==NULL)||(desc==NULL)) return TRUE; // this is primary-device

  DSoDevPair dsodevpair;
  dsodevpair.first=*guid;
  char cstr[64];
  strcpy(cstr, "[DS]");
  strcat(cstr, desc);
  dsodevpair.second=cstr;
  vfwrec->dsodevlist.push_back(dsodevpair);

  return TRUE;
}

BOOL CALLBACK VFWRec::acmDriverEnumCB(HACMDRIVERID hadid, DWORD_PTR dwparam, DWORD fdwSupport)
{
  if(fdwSupport&ACMDRIVERDETAILS_SUPPORTF_CODEC){

    VFWRec *vfwrec=(VFWRec *)dwparam;

    ACMDRIVERDETAILS driverDetails;
    driverDetails.cbStruct=sizeof(ACMDRIVERDETAILS);
    acmDriverDetails(hadid, &driverDetails, 0);
    
    HACMDRIVER had;
    acmDriverOpen(&had, hadid, 0);

    for(unsigned int i=0; i<driverDetails.cFormatTags; i++){
      ACMFORMATTAGDETAILS tagDetails;
      memset(&tagDetails, 0, sizeof(ACMFORMATTAGDETAILS));
      
      tagDetails.cbStruct=sizeof(ACMFORMATTAGDETAILS);
      tagDetails.dwFormatTagIndex=i;
      acmFormatTagDetails(had, &tagDetails, ACM_FORMATTAGDETAILSF_INDEX);

      if(tagDetails.dwFormatTag==WAVE_FORMAT_MPEGLAYER3){
        if(tagDetails.cStandardFormats>0){
          ACMDriPair acmdripair;
          acmdripair.first=hadid;
          char cstr[64];
          strcpy(cstr, tagDetails.szFormatTag);
          strcat(cstr, " (128kbps)");
          acmdripair.second=cstr;
          vfwrec->acmdrilist.push_back(acmdripair);
        }
      }
    }
    acmDriverClose(had, 0);
  }
  return TRUE;
}

#define WM_AVIRECEND WM_APP+1

LRESULT CALLBACK VFWRec::dlgProcVFWRec(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
  VFWRec *_this=(VFWRec *)GetWindowLongPtr(hdlg, GWLP_USERDATA);
  switch(msg){
  case MM_WIM_DATA:
    {
      // wavein_callback_message
      MyAVIRec_writeaudio((WAVEHDR *)lparam);
    }
    break;
    
  case WM_AVIRECEND:
    {
      KillTimer(hdlg, 0); 
      SetDlgItemText(hdlg, IDC_VFW_BUTTON1, "REC");
      CheckDlgButton(hdlg, IDC_VFW_BUTTON1, BST_UNCHECKED);
      MyAVIRec_free();
      _this->ld.printf("recorded %dframes (%.2fsec), dropped %dframes",
                       wparam, (float)wparam/15.f, lparam);
    }
    break;

  case WM_COMMAND:
    {
      switch(LOWORD(wparam)){
      case IDC_VFW_BUTTON3:
        {
          char l_foldername[MAX_PATH];
          strcpy(l_foldername, _this->cfoldername);
          bool ret=getFolderName(l_foldername, hdlg);
          if(ret==true){
            strcpy(_this->cfoldername, l_foldername);
            sprintf(_this->recfilename, "%s\\vfwrec%03d.avi",
                    _this->cfoldername, _this->recfilecount);
            SetDlgItemText(hdlg, IDC_VFW_EDIT1, _this->recfilename);
          }
        }
        break;
      case IDC_VFW_BUTTON2:
        {
          int index;
          index=(int)SendDlgItemMessage(hdlg, IDC_VFW_COMBO1, CB_GETCURSEL, 0, 0);
          if(index!=CB_ERR){
            COMPVARS compvars;
            memset(&compvars, 0, sizeof(COMPVARS));
            compvars.cbSize=sizeof(COMPVARS);
            compvars.dwFlags=ICMF_COMPVARS_VALID;
            compvars.fccHandler=_this->fourcclist[index].first;
            ICCompressorChoose(hdlg, 0, NULL, NULL, &compvars, NULL);
            ICCompressorFree(&compvars);
          }
        }
        break;
      case IDC_VFW_BUTTON1:
        {
          if(IsDlgButtonChecked(hdlg, IDC_VFW_BUTTON1)==BST_CHECKED){
            sprintf(_this->recfilename, "%s\\vfwrec%03d.avi",
                    _this->cfoldername, _this->recfilecount);
            SetDlgItemText(hdlg, IDC_VFW_EDIT1, _this->recfilename);
            _this->recfilecount++;

            DWORD fourCC=VID_UNCOMPRESSED_CODEC;
            HACMDRIVERID hmp3driverid=NULL;
            DWORD recdev=(DWORD)WAVE_MAPPER;
            GUID recdevguid=GUID_NULL;
            
            DWORD index;
            index=(DWORD)SendDlgItemMessage(hdlg, IDC_VFW_COMBO1, CB_GETCURSEL, 0, 0);
            if(index!=CB_ERR){
              fourCC=_this->fourcclist[index].first;
              _this->sel_fourCC=index;
            }

            index=(DWORD)SendDlgItemMessage(hdlg, IDC_VFW_COMBO2, CB_GETCURSEL, 0, 0);
            if(index!=CB_ERR){
              if(index<_this->mmedevlist.size()){
                recdev=_this->mmedevlist[index].first;
                recdevguid=GUID_NULL;
              }else if(index<_this->mmedevlist.size()+_this->dsodevlist.size()){
                recdev=(DWORD)WAVE_MAPPER;
                recdevguid=_this->dsodevlist[index-_this->mmedevlist.size()].first;
              }
              _this->sel_recdev=index;
            }

            index=(DWORD)SendDlgItemMessage(hdlg, IDC_VFW_COMBO3, CB_GETCURSEL, 0, 0);
            if(index!=CB_ERR){
              hmp3driverid=_this->acmdrilist[index].first;
              _this->sel_acmformat=index;
            }

            int capture_b;

            if(IsDlgButtonChecked(hdlg, IDC_VFW_RADIO1)==BST_CHECKED){
              capture_b=24;
            }else{
              capture_b=32;
            }

            DWORD dwrop=SRCCOPY;
            if(IsDlgButtonChecked(hdlg, IDC_VFW_CHECK1)==BST_CHECKED){
              dwrop|=CAPTUREBLT;
            }

            int capture_x, capture_y;
            int capture_w, capture_h;

            if(IsDlgButtonChecked(hdlg, IDC_VFW_RADIO3)==BST_CHECKED){
              capture_x=_this->cx;
              capture_y=_this->cy;
              capture_w=_this->cw;
              capture_h=_this->ch;
            }else if(IsDlgButtonChecked(hdlg, IDC_VFW_RADIO5)==BST_CHECKED){
              capture_x=_this->cx+_this->cw/2-640/2;
              capture_y=_this->cy+_this->ch/2-480/2;
              capture_w=640;
              capture_h=480;
            }else{
              RECT rc;
              GetClientRect(_this->hwndTar, &rc);
              capture_x=0;
              capture_y=0;
              capture_w=rc.right;
              capture_h=rc.bottom;
            }

            if(_this->ret_fourCC!=NULL)    *_this->ret_fourCC   =_this->sel_fourCC;
            if(_this->ret_recdev!=NULL)    *_this->ret_recdev   =_this->sel_recdev;
            if(_this->ret_acmformat!=NULL) *_this->ret_acmformat=_this->sel_acmformat;
            
            bool ret=MyAVIRec_start(_this->recfilename,
                                    recdev, &recdevguid,
                                    fourCC, hmp3driverid, 
                                    _this->hwndTar, dwrop,
                                    capture_x, capture_y, capture_w, capture_h, capture_b,
                                    hdlg, WM_AVIRECEND, 0);
            if(ret==true){
              SetTimer(hdlg, 0, 500, 0);
              _this->m_starttime=timeGetTime();
              SetDlgItemText(hdlg, IDC_VFW_BUTTON1, "00:00");
            }

          }else{
            KillTimer(hdlg, 0); 
            SetDlgItemText(hdlg, IDC_VFW_BUTTON1, "REC");
            CheckDlgButton(hdlg, IDC_VFW_BUTTON1, BST_UNCHECKED);
            if(MyAVIRec_recording()==true) MyAVIRec_stop();
          }
        }
        return TRUE;
        break;
      }
    }
    break;
  case WM_TIMER:
    {
      char btext[16];
      DWORD currenttime=timeGetTime();
      DWORD elappsedsec=(currenttime-_this->m_starttime)/1000;
      DWORD m=elappsedsec/60;
      DWORD s=elappsedsec%60;
      sprintf(btext, "%02d:%02d", m, s);
      SetDlgItemText(hdlg, IDC_VFW_BUTTON1, btext);
    }
    break;
    
  case WM_CLOSE:
    if(MyAVIRec_recording()==true){
      KillTimer(hdlg, 0); 
      SetDlgItemText(hdlg, IDC_VFW_BUTTON1, "REC");
      CheckDlgButton(hdlg, IDC_VFW_BUTTON1, BST_UNCHECKED);
      MyAVIRec_stop();
      SetWindowLongPtr(hdlg, DWL_MSGRESULT, FALSE);
    }else{
      DestroyWindow(hdlg);
      SetWindowLongPtr(hdlg, DWL_MSGRESULT, TRUE);
    }
    break;

  case WM_DESTROY:
    _this->m_hdlg=NULL;
    break;

  case WM_INITDIALOG:
    {
      _this=(VFWRec *)lparam;
      SetWindowLongPtr(hdlg, GWLP_USERDATA, (LONG_PTR)_this);

      sprintf(_this->recfilename, "%s\\vfwrec%03d.avi",
              _this->cfoldername, _this->recfilecount);
      SetDlgItemText(hdlg, IDC_VFW_EDIT1, _this->recfilename);

      CheckRadioButton(hdlg, IDC_VFW_RADIO1, IDC_VFW_RADIO2, IDC_VFW_RADIO2);
      CheckRadioButton(hdlg, IDC_VFW_RADIO3, IDC_VFW_RADIO5, IDC_VFW_RADIO3);
      CheckRadioButton(hdlg, IDC_VFW_RADIO6, IDC_VFW_RADIO6, IDC_VFW_RADIO6);

      {
        // video codec
        FourCCPair fourccpair;

        fourccpair.first=VID_UNCOMPRESSED_CODEC;
        fourccpair.second="uncompressed";
        _this->fourcclist.push_back(fourccpair);

        for(int i=0;;i++){
          ICINFO icinfo;
          if(!ICInfo(ICTYPE_VIDEO, i, &icinfo)) break;
          HIC hic=ICOpen(ICTYPE_VIDEO, icinfo.fccHandler, ICMODE_QUERY);
          ICGetInfo(hic, &icinfo, sizeof(ICINFO));
          ICClose(hic);
          if(icinfo.dwFlags>0){
            char desc[128];
            memset(desc, 0, 128);
            WideCharToMultiByte(CP_ACP, 0, icinfo.szDescription, -1,
                                desc, 128, NULL, NULL);
            fourccpair.first=icinfo.fccHandler;
            fourccpair.second=desc;
            _this->fourcclist.push_back(fourccpair);
          }
        }
        
        for(DWORD i=0; i<_this->fourcclist.size(); i++){
          SendDlgItemMessage(hdlg, IDC_VFW_COMBO1, CB_INSERTSTRING, i,
                             (LPARAM)_this->fourcclist[i].second.c_str());
        }
        if(_this->sel_fourCC>=_this->fourcclist.size()) _this->sel_fourCC=0;
        SendDlgItemMessage(hdlg, IDC_VFW_COMBO1, CB_SETCURSEL, _this->sel_fourCC, 0);
      }


      {
        // recroding device
        // mme
        MMEDevPair mmedevpair;
        mmedevpair.first=(DWORD)WAVE_MAPPER-1;
        mmedevpair.second="(none)";
        _this->mmedevlist.push_back(mmedevpair);
        mmedevpair.first=(DWORD)WAVE_MAPPER;
        mmedevpair.second="default MIC device";
        _this->mmedevlist.push_back(mmedevpair);
        
        for(DWORD i=0; i<waveInGetNumDevs(); i++){
          WAVEINCAPS caps;
          if(waveInGetDevCaps(i, &caps, sizeof(caps))==MMSYSERR_NOERROR){
            mmedevpair.first=i;
            mmedevpair.second=caps.szPname;
            _this->mmedevlist.push_back(mmedevpair);
          }
        }
        
        for(DWORD i=0; i<_this->mmedevlist.size(); i++){
          SendDlgItemMessage(hdlg, IDC_VFW_COMBO2, CB_INSERTSTRING, i,
                             (LPARAM)_this->mmedevlist[i].second.c_str());
        }

        // directsound
        DirectSoundCaptureEnumerate(dSoundEnumCB, (LPVOID)_this);
        for(DWORD i=0; i<_this->dsodevlist.size(); i++){
          SendDlgItemMessage(hdlg, IDC_VFW_COMBO2, CB_INSERTSTRING, i+_this->mmedevlist.size(),
                             (LPARAM)_this->dsodevlist[i].second.c_str());
        }
        if(_this->sel_recdev>=_this->mmedevlist.size()+_this->dsodevlist.size()){
          _this->sel_recdev=0;
        }
        SendDlgItemMessage(hdlg, IDC_VFW_COMBO2, CB_SETCURSEL, _this->sel_recdev, 0);

      }


      {
        // audio codec
        ACMDriPair acmdripair;
        acmdripair.first=NULL;
        acmdripair.second="PCM";
        _this->acmdrilist.push_back(acmdripair);
        
        acmDriverEnum((ACMDRIVERENUMCB)acmDriverEnumCB, (DWORD)_this, 0);
        
        for(DWORD i=0; i<_this->acmdrilist.size(); i++){
          SendDlgItemMessage(hdlg, IDC_VFW_COMBO3, CB_INSERTSTRING, i,
                             (LPARAM)_this->acmdrilist[i].second.c_str());
        }
        if(_this->sel_acmformat>=_this->acmdrilist.size()) _this->sel_acmformat=0;
        SendDlgItemMessage(hdlg, IDC_VFW_COMBO3, CB_SETCURSEL, _this->sel_acmformat, 0);
      }

      return TRUE;
    }
    break;
  }
  return FALSE;
}

HWND VFWRec::DialogOpen(HWND hwnd, HWND hwnd_uoclist, char *foldername,
                        int x, int y, int w, int h,
                        DWORD *last_fourCC, DWORD *last_recdev, DWORD *last_acmformat)
{
  if(m_hdlg==NULL){
    cfoldername=foldername;
    sprintf(recfilename, "%s\\vfwrec%03d.avi", cfoldername, recfilecount);
    SetDlgItemText(m_hdlg, IDC_VFW_EDIT1, recfilename);

    hwndTar=hwnd;
    cx=x; cy=y; cw=w; ch=h;
    
    fourcclist.clear();
    mmedevlist.clear();
    dsodevlist.clear();
    acmdrilist.clear();

    if(last_fourCC!=NULL){
      sel_fourCC=*last_fourCC;
      ret_fourCC=last_fourCC;
    }else{
      sel_fourCC=0;
    }
    if(last_recdev!=NULL){
      sel_recdev=*last_recdev;
      ret_recdev=last_recdev;
    }else{
      sel_recdev=0;
    }
    if(last_acmformat!=NULL){
      sel_acmformat=*last_acmformat;
      ret_acmformat=last_acmformat;
    }else{
      sel_acmformat=0;
    }

    m_hdlg=CreateDialogParam(m_hAppInstance, MAKEINTRESOURCE(IDD_DIALOGVFW), hwnd_uoclist,
                             (DLGPROC)dlgProcVFWRec, (LONG_PTR)this);
    return m_hdlg;
  }else{
    return NULL;
  }
}

