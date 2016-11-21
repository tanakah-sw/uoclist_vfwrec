// logdisp.h

#ifndef LOGDISP
#define LOGDISP

#pragma warning(disable: 4996) // warning safe_stdio
#include <stdio.h>
#define _USE_MATH_DEFINES // use M_PI
#include <math.h>

#include <windows.h>
#include <richedit.h>

// LF    \n     0A
// CR    \r     0D
// CR+LF \r\n   0D 0A

class LogDisp{
public:
  LogDisp() { hwndRE=NULL; memset(MText, 0, 1024); };
  ~LogDisp() {};

  void setOutputWnd(HWND hwnd) { hwndRE=hwnd; }

  void clear() { memset(MText, 0, 1024); }
  void flush() { writeWnd(false); }

  void strcpy(char *txt) { ::strcpy(MText, txt); }
  void strcat(char *txt) { ::strcat(MText, txt); }
  void sprintf(char *fmt, ...)
  {
    char **lppParam;
    lppParam=((char **)&fmt)+1;
    vsprintf(MText, fmt, (va_list)lppParam);
  }

  void LogDisp::printf(char *fmt, ...)
  {
    char **lppParam;
    lppParam=((char **)&fmt)+1;
    vsprintf(MText, fmt, (va_list)lppParam);
    writeWnd(false);
  }

  void LogDisp::writeDbg()
  {
    OutputDebugString(MText);
    char LF[2]={'\n', 0};
    OutputDebugString(LF);
  }
  
  void LogDisp::writeWnd(bool cautioncolor=false)
  {
    if(hwndRE==NULL) writeDbg();
    
    int oldstart, oldend;
    SendMessage(hwndRE, EM_GETSEL, (WPARAM)&oldstart, (LPARAM)&oldend);
    
    const int nlinebuf=1024;
    unsigned char line[nlinebuf];
    int lines=(int)SendMessage(hwndRE, EM_GETLINECOUNT, 0, 0);
    int len=0;
    if(lines>nlinebuf){
      line[0]=0xfe; line[1]=0x04;
      len=(int)SendMessage(hwndRE, EM_GETLINE, 0, (LPARAM)line);
      if(len!=0){
        SendMessage(hwndRE, EM_SETSEL, 0, (LPARAM)len);
        SendMessage(hwndRE, EM_REPLACESEL, 0, (LPARAM)"");
      }
    }
    DWORD index=(DWORD)SendMessage(hwndRE, WM_GETTEXTLENGTH, 0, 0);
    SendMessage(hwndRE, EM_SETSEL, index, (LPARAM)index);
    
    CHARFORMAT2 cf;
    cf.cbSize=sizeof(cf);
    cf.dwMask=CFM_COLOR;
    SendMessage(hwndRE, EM_GETCHARFORMAT, (WPARAM)SCF_SELECTION, (LPARAM)&cf);
    COLORREF cr;
    cr=(cautioncolor==true) ? RGB(255, 0, 0) : RGB(0, 0, 0);
    cf.dwEffects&=~CFE_AUTOCOLOR;
    cf.crTextColor=cr;
    SendMessage(hwndRE, EM_SETCHARFORMAT, (WPARAM)SCF_SELECTION, (LPARAM)&cf);
    
    ::strcpy((char *)line, MText);
    ::strcat((char *)line, "\r\n");
    SendMessage(hwndRE, EM_REPLACESEL, 0, (LPARAM)line);
    
    oldstart=oldstart-len;
    oldend=oldend-len;
    if(oldend<0){
      oldstart=-1;
      oldend=0;
    }else{
      if(oldstart<0) oldstart=0;
    }
    SendMessage(hwndRE, EM_SETSEL, (WPARAM)oldstart, (LPARAM)oldend);
    SendMessage(hwndRE, WM_VSCROLL, SB_BOTTOM, 0);
  }
protected:
  HWND hwndRE;
  char MText[1024];
};

#endif
