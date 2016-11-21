// vfwutil.h
#ifndef VFWREC
#define VFWREC

// STL
#include <vector>

// MME/ACM
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "msacm32.lib")

// VFW
#include <vfw.h>
#pragma comment(lib, "vfw32.lib")

// DirectSound
#include <dsound.h>
#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "dxguid.lib")

#include "logdisp.h"

typedef std::pair<DWORD, char [64]> FourCCPair;
typedef std::vector<FourCCPair> FourCCList;

typedef std::pair<DWORD, char [64]> MMEDevPair;
typedef std::vector<MMEDevPair> MMEDevList;

typedef std::pair<GUID, char [64]> DSoDevPair;
typedef std::vector<DSoDevPair> DSoDevList;

typedef std::pair<HACMDRIVERID, char [64]> ACMDriPair;
typedef std::vector<ACMDriPair> ACMDriList;


class VFWRec
{
public:
  VFWRec():
    m_hAppInstance(0), m_hdlg(0)
  {}
  ~VFWRec();
  void Init(HINSTANCE hinstance, HWND hwndRE)
  {
    m_hAppInstance=hinstance;
    ld.setOutputWnd(hwndRE);
  }
  bool DialogOpen(HWND hwnd, char *folderanme,
                  DWORD *last_fourCC, DWORD *last_recdev, DWORD *last_acmformat);
  bool RecStart();
  void Close()
  {
    if(m_hdlg!=NULL){
      DWORD ret=SendMessage(m_hdlg, WM_CLOSE, 0, 0);
      if(ret==FALSE) SendMessage(m_hdlg, WM_CLOSE, 0, 0);
    }
  }

private:
  static LRESULT CALLBACK dlgProcVFWRec(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam);
  static BOOL CALLBACK acmDriverEnumCB(HACMDRIVERID hadid, DWORD_PTR dwparam, DWORD fdwSupp);
  static BOOL CALLBACK dSoundEnumCB(LPGUID lpguid, LPCSTR desc, LPCSTR module, LPVOID user);

  HINSTANCE m_hAppInstance;
  HWND m_hdlg;

  LogDisp ld;
  
  int  recfilecount;
  char recfoldername[MAX_PATH];
  char recfilename[MAX_PATH];

  DWORD m_starttime;

  // targetwindow
  HWND hwndTar;

  // encoder/device list
  FourCCList fourcclist;
  MMEDevList mmedevlist;
  DSoDevList dsodevlist;
  ACMDriList acmdrilist;
  
  DWORD  sel_fourCC,  sel_recdev,  sel_acmformat;
  DWORD *ret_fourCC, *ret_recdev, *ret_acmformat;
};

#endif
