// this file may be easier to customize if included directly from other code. Some things that can be defined to hook:
//
// #define WDL_LOCALIZE_HOOK_ALLOW_CACHE (ismainthread())
// #define WDL_LOCALIZE_HOOK_DLGPROC if (is_modal) return __localDlgProcModalPos; // can return a temporary dlgproc override
// #define WDL_LOCALIZE_HOOK_XLATE(str,subctx,flags,newptr) // can be used to override/tweak translations (newptr str is the original string, newptr is the translated string (or NULL) )
#ifdef _WIN32
#include <windows.h>
#else
#include "../swell/swell.h"
#endif

#ifndef LOCALIZE_NO_DIALOG_MENU_REDEF
#define LOCALIZE_NO_DIALOG_MENU_REDEF
#endif
#include "localize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../assocarray.h"
#include "../ptrlist.h"
#include "../chunkalloc.h"
#include "../fnv64.h"
#include "../win32_utf8.h"
#include "../wdlcstring.h"

#ifndef WDL_LOCALIZE_REC_HEADER_SIZE
#define WDL_LOCALIZE_REC_HEADER_SIZE 0
#endif
#ifndef WDL_LOCALIZE_UPDATE_HEADER
#define WDL_LOCALIZE_UPDATE_HEADER(hdr,v) do { } while(0)
#endif

#define LANGPACK_SCALE_CONSTANT WDL_UINT64_CONST(0x5CA1E00000000000)
static WDL_StringKeyedArray< WDL_KeyedArray<WDL_UINT64, char *> * > g_translations;
static WDL_KeyedArray<WDL_UINT64, char *> *g_translations_commonsec;

static bool isPrintfModifier(char c)
{
  switch (c)
  {
    case '.':
    case '-':
    case '+':
    case '#':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
    case ' ':
      return true;
  }
  return false;
}

static const char *next_format(const char *p)
{
  for (;;)
  {
    switch (p[0])
    {
      case 0: return p;
      case '%':
        if (p[1] != '%') return p;
        p++;
        WDL_FALLTHROUGH;
      default:
        p++;
      break;
    }
  }
}

static bool validateStrs(const char *stock, const char *newstr, int flags)
{
  if (!(flags&LOCALIZE_FLAG_VERIFY_FMTS)) return true;

  for (;;)
  {
    newstr = next_format(newstr);
    if (!newstr[0]) return true; // not enough formats in new string, safe
    stock = next_format(stock);
    if (!stock[0]) return false; // not enough formats in original string, unsafe

    // compare formats
    newstr++;
    stock++;

    do
    {
      while (isPrintfModifier(*newstr)) newstr++;
      while (isPrintfModifier(*stock)) stock++;
      if (*newstr != *stock) return false;
      newstr++;
      stock++;
    } while (newstr[-1] == '*' || newstr[-1] == 'h' || newstr[-1] == 'l' || newstr[-1] == 'L');
  }
  return true;
}

static char *ChunkAlloc(int len)
{
#ifdef _DEBUG
  static WDL_ChunkAlloc backing(15000); // will free on exit (for debug mode)
  return (char*)backing.Alloc(len);
#else
  static WDL_ChunkAlloc *backing;
  if (!backing) backing = new WDL_ChunkAlloc(15000);
  return (char*)backing->Alloc(len);
#endif
}

int wdl_localize_options; // &1=show untranslated in menus

#ifdef _DEBUG
  bool g_debug_langpack_has_loaded;

  class localizeValidatePtrCacheRec {
    public:
      localizeValidatePtrCacheRec(const char *subctx, const char *str, const char *ctx)
      {
        //printf("saving pointers for validation for %s (%s)\n",str,ctx);
        m_str_copy=strdup(m_str=str);
        m_sub_copy=strdup(m_sub=subctx);
        m_ctx=ctx;
      }
      ~localizeValidatePtrCacheRec()
      {
        //printf("validating string %s\n",m_str_copy);
        WDL_ASSERT(!strcmp(m_sub,m_sub_copy) /* if this triggers, then a caller called __LOCALIZE without NOCACHE on a non-static or unloaded string */ );
        WDL_ASSERT(!strcmp(m_str,m_str_copy) /* if this triggers, then a caller called __LOCALIZE without NOCACHE on a non-static or unloaded string */ );
      }
      char *m_sub_copy, *m_str_copy;
      const char *m_sub, *m_str, *m_ctx;
  };
  static WDL_PtrList_DeleteOnDestroy<localizeValidatePtrCacheRec> s_debug_validateptrcache;
  #define  LANGPACK_DEBUG_SAVE_POINTERS_FOR_VALIDATION(subctx,str,ctx) \
    s_debug_validateptrcache.Add(new localizeValidatePtrCacheRec(subctx,str,ctx));
#else
  #define LANGPACK_DEBUG_SAVE_POINTERS_FOR_VALIDATION(subctx,str,ctx)
#endif

const char *__localizeFunc(const char *str, const char *subctx, int flags)
{
#ifdef _DEBUG
  WDL_ASSERT(g_debug_langpack_has_loaded != false);
#endif
  if (WDL_NOT_NORMALLY(!str || !subctx || !subctx[0])) return str;
  if (!g_translations.GetSize())
  {
#ifdef WDL_LOCALIZE_HOOK_XLATE
    char *newptr=NULL;
    WDL_LOCALIZE_HOOK_XLATE(str,subctx,flags,newptr)
    if (newptr) return newptr;
#endif
    return str;
  }

#ifdef WDL_LOCALIZE_HOOK_ALLOW_CACHE
  const bool cache_lookups = !(flags&LOCALIZE_FLAG_NOCACHE) && WDL_LOCALIZE_HOOK_ALLOW_CACHE;
#else
  const bool cache_lookups = !(flags&LOCALIZE_FLAG_NOCACHE);
#endif

#ifdef BENCHMARK_I8N
  static int stats[2];
  static double sumtimes[2]; // [cached]
  double startTime=time_precise();
#endif

  static WDL_PtrKeyedArray< WDL_PtrKeyedArray<char *> * > ptrcache;
  WDL_PtrKeyedArray<char *> *sc = NULL;

  if (cache_lookups)
  {
    sc = ptrcache.Get((INT_PTR)subctx);
    if (sc)
    {
      const char *ret = sc->Get((INT_PTR)str);
      if (ret)
      {
#ifdef BENCHMARK_I8N
        stats[1]++;
        startTime = time_precise()-startTime;
        sumtimes[1]+=startTime;
        if (!(stats[1]%100))
        {
          wdl_log("cached, avg was %fuS\n",sumtimes[1]*10000.0);
          sumtimes[1]=0;
        }
#endif

        if (ret == (const char *)(INT_PTR)1) return str;
        return ret;
      }
    }
  }

  char *newptr = NULL;

  int trycnt;
  size_t len = strlen(str)+1;

  if (flags & LOCALIZE_FLAG_DOUBLENULL)
  {
    // need to test this
    for (;;)
    {
      size_t a = strlen(str+len);
      if (!a) break;
      len += a+1;
    }
    len++;
  }
  else if (flags & LOCALIZE_FLAG_PAIR)
  {
    len += strlen(str + len) + 1;
  }

  WDL_UINT64 hash;
  if ((flags & LOCALIZE_FLAG_PAIR) && len == 18 && !memcmp(str,"__LOCALIZE_SCALE\0",18) && !(wdl_localize_options&2))
    hash = WDL_UINT64_CONST(0x5CA1E00000000000);
  else
    hash = WDL_FNV64(WDL_FNV64_IV,(const unsigned char *)str,(int)len);

  for (trycnt=0;trycnt<2 && !newptr;trycnt++)
  {
    WDL_KeyedArray<WDL_UINT64, char *> *section = trycnt == 1 ? g_translations_commonsec : g_translations.Get(subctx);

    if (section)
    {
      newptr = section->Get(hash,0);
      if (newptr && !validateStrs(str,newptr,flags)) newptr=NULL;

      if (hash != WDL_UINT64_CONST(0x5CA1E00000000000))
      {
        WDL_LOCALIZE_UPDATE_HEADER((char*)newptr,str);
      }
    }
  }

  #ifdef WDL_LOCALIZE_HOOK_XLATE
  WDL_LOCALIZE_HOOK_XLATE(str,subctx,flags,newptr)
  #endif

  if (cache_lookups)
  {
    if (!sc)
    {
      sc = new WDL_PtrKeyedArray<char *>;
      ptrcache.Insert((INT_PTR)subctx,sc);
    }
    sc->Insert((INT_PTR)str,newptr ? newptr : (char*)(INT_PTR)1); // update pointer cache for fast lookup
    LANGPACK_DEBUG_SAVE_POINTERS_FOR_VALIDATION(subctx,str,"")
  }
#ifdef BENCHMARK_I8N
  stats[0]++;
  startTime = time_precise()-startTime;
  sumtimes[0]+=startTime;
  if (!(stats[0]%100))
  {
    wdl_log("uncached , avg was %f\n",sumtimes[0]*10000.0);
    sumtimes[0]=0;
  }
#endif

  return newptr?newptr:str;
}

static void __localProcMenu(HMENU menu, WDL_KeyedArray<WDL_UINT64, char *> *s)
{
  int n = GetMenuItemCount(menu);
  int x;
  char buf[4096];
  for(x=0;x<n;x++)
  {
    buf[0]=0;
    MENUITEMINFO mii={sizeof(mii),MIIM_TYPE|MIIM_SUBMENU,0,0,0,0,0,0,0,buf,sizeof(buf)};
    if (GetMenuItemInfo(menu,x,TRUE,&mii))
    {
      if (mii.fType == MFT_STRING)
      {
        buf[sizeof(buf)-1]=0;

  #ifdef _WIN32
        char mod[512];
        mod[0]=0;
        tryagain:
  #endif
        WDL_UINT64 hash = WDL_FNV64(WDL_FNV64_IV,(const unsigned char *)buf,(int)strlen(buf)+1);
        const char *newptr = s ? s->Get(hash,0) : NULL;
        if (!newptr && g_translations_commonsec) newptr = g_translations_commonsec->Get(hash,0);
        WDL_LOCALIZE_UPDATE_HEADER((char*)newptr,buf);

        if (!newptr)
        {
          #ifdef _WIN32
            char *p =buf;
            while (*p && *p != '\t') p++;
            lstrcpyn(mod,p,sizeof(mod));
            if (*p)
            {
              *p++=0;
              goto tryagain;
            }
          #endif

        }
        if (newptr)
        {
#ifndef _WIN32
          const char mod[2]={0,};
#endif
          if (mod[0] || (wdl_localize_options&1))
          {
            const int maxl = mod[0] ? 3500 : 4000;
            if (wdl_localize_options&1)
            {
              int l = (int)strlen(newptr);
              int l2 = (int)strlen(buf);
              if (l2 > 500) l2=500;
              if (l > maxl-l2) l = maxl-l2;
              memmove(buf+l+3, buf, l2+1);
              memcpy(buf,newptr,l);
              memcpy(buf+l," | ",3);
            }
            else
              lstrcpyn(buf,newptr,maxl);
            if (mod[0]) strcat(buf,mod);
            newptr=buf;
          }
          mii.fMask = MIIM_TYPE;
#ifdef __APPLE__
          mii.fMask |= MIIM_SWELL_DO_NOT_CALC_MODIFIERS;
#endif
          mii.dwTypeData = (char*)newptr;
          SetMenuItemInfo(menu,x,TRUE,&mii);
        }
      }

      if (mii.hSubMenu) __localProcMenu(mii.hSubMenu,s);
    }
  }
}

void __localizeMenu(const char *rescat, HMENU hMenu, LPCSTR lpMenuName)
{
#ifdef _DEBUG
  WDL_ASSERT(g_debug_langpack_has_loaded != false);
#endif
  INT_PTR a = (INT_PTR) lpMenuName;
  if (hMenu && a>0&&a<65536)
  {
    char buf[128];
    sprintf(buf,"%sMENU_%d",rescat?rescat:"",(int)a);
    WDL_KeyedArray<WDL_UINT64, char *> *s = g_translations.Get(buf);
    if (s)
    {
      __localProcMenu(hMenu,s);
    }
  }
}

HMENU __localizeLoadMenu(HINSTANCE hInstance, LPCSTR lpMenuName)
{
  HMENU ret = LoadMenu(hInstance,lpMenuName);
  if (ret) __localizeMenu(NULL,ret,lpMenuName);
  return ret;
}

struct windowReorgEnt
{
  enum windowReorgEntType
  {
    WRET_GROUP=0,
    WRET_SIZEADJ,
    WRET_MISC, // dont analyze for size changes, but move around

  };
  windowReorgEnt(HWND _hwnd, RECT _r, int wc)
  {
    hwnd=_hwnd;
    orig_r=r=_r;
    mode=WRET_MISC;
    move_amt=0;
    wantsizeincrease=0;
    scaled_width_change = wc;
    wnd_id = GetWindowLong(hwnd,GWL_ID);
  }
  ~windowReorgEnt() { }

  HWND hwnd;
  RECT r,orig_r;
  windowReorgEntType mode;
  int move_amt;
  int wantsizeincrease;
  int scaled_width_change;
  int wnd_id;

  static int Sort(const void *_a, const void *_b)
  {
    const windowReorgEnt *a = (const windowReorgEnt*)_a;
    const windowReorgEnt *b = (const windowReorgEnt*)_b;

    if (a->r.left < b->r.left) return -1;
    if (a->r.left > b->r.left) return 1;

    if (a->r.top < b->r.top) return -1;
    if (a->r.top > b->r.top) return 1;

    return 0;
  }

};
class windowReorgState
{
public:

  windowReorgState(HWND _par, float _scx, float _scy) : par(_par), scx(_scx), scy(_scy)
  {
    RECT cr;
    GetClientRect(_par,&cr);

    par_cr = cr;
    if (scx != 1.0f) par_cr.right = (int) (par_cr.right * scx + 0.5);
    if (scy != 1.0f) par_cr.bottom = (int) (par_cr.bottom * scy + 0.5);

    has_sc = (cr.right != par_cr.right || cr.bottom != par_cr.bottom);

    if (has_sc)
    {
      RECT wr;
      if (GetWindowLong(_par,GWL_STYLE)&WS_CHILD) wr=cr;
      else GetWindowRect(_par,&wr);

      SetWindowPos(_par,NULL,0,0,
                            (par_cr.right - cr.right) + (wr.right-wr.left),
                            (par_cr.bottom - cr.bottom) + (wr.bottom-wr.top),
                            SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);
    }
  }
  ~windowReorgState() { }

  WDL_TypedBuf<windowReorgEnt> cws;
  WDL_IntKeyedArray<int> columns; // map l|(r<<16) -> maps to size increase
  RECT par_cr;

  HWND par;
  float scx,scy;
  bool has_sc;
};

static const char *xlateWindow(HWND hwnd, WDL_KeyedArray<WDL_UINT64, char *> *s, char *buf, int bufsz, bool prefix_handling)
{
  buf[0]=0;
  GetWindowText(hwnd,buf,bufsz);
  if (buf[0])
  {
    buf[bufsz-1]=0;
    WDL_UINT64 hash = WDL_FNV64(WDL_FNV64_IV,(const unsigned char *)buf,(int)strlen(buf)+1);
    const char *newptr = s ? s->Get(hash,0) : NULL;
    if (!newptr && g_translations_commonsec) newptr = g_translations_commonsec->Get(hash,0);
    WDL_LOCALIZE_UPDATE_HEADER((char*)newptr,buf);

#ifdef __APPLE__
    bool filter_prefix = false;
    if (!newptr && prefix_handling)
    {
      extern const char *SWELL_GetRecentPrefixRemoval(const char *p);
      const char *p = SWELL_GetRecentPrefixRemoval(buf);
      if (p)
      {
        hash = WDL_FNV64(WDL_FNV64_IV,(const unsigned char *)p,(int)strlen(p)+1);
        newptr = s ? s->Get(hash,0) : NULL;
        if (!newptr && g_translations_commonsec) newptr = g_translations_commonsec->Get(hash,0);
        WDL_LOCALIZE_UPDATE_HEADER((char*)newptr,buf);
        filter_prefix = true;
      }
    }
#endif

    if (newptr && strcmp(newptr,buf))
    {
#ifdef __APPLE__
      if (filter_prefix)
      {
        const char *rd=newptr;
        int widx=0;
        while (widx < bufsz-1)
        {
          if (*rd == '&') rd++;
          if (!*rd) break;
          buf[widx++]=*rd++;
        }
        buf[widx]=0;
        SetWindowText(hwnd,buf);
        return newptr;
      }
#endif
      SetWindowText(hwnd,newptr);
      return newptr;
    }
  }
  return NULL;
}

static BOOL CALLBACK xlateGetRects(HWND hwnd, LPARAM lParam)
{
  windowReorgState *s=(windowReorgState*)lParam;

  if (GetParent(hwnd) != s->par) return TRUE;

  RECT r;
  GetWindowRect(hwnd,&r);
  ScreenToClient(s->par,(LPPOINT)&r);
  ScreenToClient(s->par,((LPPOINT)&r)+1);
  int width_change = 0;

  if (s->has_sc) // scaling happens before all of the ripple-code
  {
    if (r.top > r.bottom) { const int t = r.top; r.top = r.bottom; r.bottom = t; }

    width_change = r.right-r.left;
    r.left = (int) (r.left * s->scx + 0.5);
    r.top = (int) (r.top * s->scy + 0.5);
    r.right = (int) (r.right * s->scx + 0.5);
    r.bottom = (int) (r.bottom * s->scy + 0.5);
    SetWindowPos(hwnd,NULL, r.left,r.top, r.right-r.left, r.bottom-r.top, SWP_NOACTIVATE|SWP_NOZORDER);
    width_change = (r.right-r.left) - width_change;
  }

  windowReorgEnt t(hwnd,r,width_change);

#ifdef _WIN32
  char buf[128];
  buf[0]=0;
  GetClassName(hwnd,buf,sizeof(buf));
  if (!strcmp(buf,"Button"))
  {
    LONG style=GetWindowLong(hwnd,GWL_STYLE);
    if (LOWORD(style)==BS_GROUPBOX) t.mode = windowReorgEnt::WRET_GROUP;
    else t.mode = windowReorgEnt::WRET_SIZEADJ;
  }
  else if (!strcmp(buf,"Static"))
  {
    if (!(GetWindowLong(hwnd,GWL_STYLE)&(SS_RIGHT|SS_CENTER)))
      t.mode = windowReorgEnt::WRET_SIZEADJ;
  }
#else
  if (SWELL_IsGroupBox(hwnd)) t.mode = windowReorgEnt::WRET_GROUP;
  else if (SWELL_IsButton(hwnd)) t.mode = windowReorgEnt::WRET_SIZEADJ;
  else if (SWELL_IsStaticText(hwnd))
  {
    if (!(GetWindowLong(hwnd,GWL_STYLE)&(SS_RIGHT|SS_CENTER))) t.mode = windowReorgEnt::WRET_SIZEADJ;
  }
#endif

  if (t.mode == windowReorgEnt::WRET_GROUP)
  {
    // first copy is the left side of the group, with hwnd
    int a = t.r.right;
    t.r.right = t.r.left+4;
    t.orig_r=t.r;
    s->cws.Add(t);

    // add a second copy, which is the right side but no hwnd
    t.r.right = a;
    t.r.left = a-4;
    t.hwnd=0;
  }
  t.orig_r=t.r;
  s->cws.Add(t);

  return TRUE;
}

static int rippleControlsRight(HWND hwnd, const RECT *srcR, windowReorgEnt *ent, int ent_cnt, int dSize, int clientw)
{
  // limit first to client rectangle
  int space = clientw - 6 - srcR->right;
  if (dSize>space) dSize=space;

  while (ent_cnt>0 && dSize>0)
  {
    // if the two items overlap by more than 1px initially, dont make them ripple (it is assumed they are allowed, maybe one is hidden)
#define LOCALIZE_ISECT_PAIRS(x1,x2,y1,y2)  \
      ((x1 >= y1 && x1 < y2) || (x2 >= y1 && x2 < y2) ||  \
       (y1 >= x1 && y1 < x2) || (y2 >= x1 && y2 < x2))
    if (ent->r.left >= srcR->right-1 && LOCALIZE_ISECT_PAIRS(srcR->top,srcR->bottom,ent->r.top,ent->r.bottom))
    {
      space = ent->r.left - srcR->right;

      if (ent->mode == windowReorgEnt::WRET_GROUP)
      {
        if (dSize>space) dSize=space;
      }
      else
      {
        int needAmt = dSize - (space-2 /* border */);
        if (needAmt>0)
        {
          int got = rippleControlsRight(ent->hwnd,&ent->r,ent+1,ent_cnt-1,needAmt,clientw);
          dSize -= needAmt - got;
          if (ent->move_amt < got) ent->move_amt=got;
        }
      }
    }

    ent_cnt--;
    ent++;
  }
  return dSize;
}

struct ctl_scale_info
{
  float xsc;
  float xadj;
  float ysc;
};

static void localize_dialog(HWND hwnd, WDL_KeyedArray<WDL_UINT64, char *> *sec)
{
#ifdef _DEBUG
  WDL_ASSERT(g_debug_langpack_has_loaded != false);
#endif
  const char *sc_str = (wdl_localize_options&2) ? NULL : sec->Get(LANGPACK_SCALE_CONSTANT);
/* commented [common] scaling fallback: the langpack author has to set it for each dialog that needs it
          if (!sc_str && g_translations_commonsec)
          {
            sc_str = g_translations_commonsec->Get(LANGPACK_SCALE_CONSTANT);
          }
*/
  bool auto_expand = false;
  float scx = 1.0, scy = 1.0;
  WDL_IntKeyedArray<ctl_scale_info> ctl_scales;
  if (sc_str)
  {
    while (*sc_str && (*sc_str == ' ' || *sc_str == '\t')) sc_str++;

#ifdef WDL_HOOK_LOCALIZE_ATOF
    float v = (float) WDL_HOOK_LOCALIZE_ATOF(sc_str);
#else
    float v = (float) atof(sc_str);
#endif
    if (v > 0.1 && v < 8.0)
    {
      scx=v;
      while (*sc_str && *sc_str != ' ' && *sc_str != '\t') sc_str++;
      while (*sc_str && (*sc_str == ' ' || *sc_str == '\t')) sc_str++;
#ifdef WDL_HOOK_LOCALIZE_ATOF
      v = (float)WDL_HOOK_LOCALIZE_ATOF(sc_str);
#else
      v = (float)atof(sc_str);
#endif
      if (v > 0.1 && v < 8.0)
      {
        scy = v;
        while (*sc_str && *sc_str != ' ' && *sc_str != '\t') sc_str++;
      }
    }
    while (*sc_str)
    {
      while (*sc_str && (*sc_str == ' ' || *sc_str == '\t')) sc_str++;
      if (*sc_str == '@')
      {
        int id = atoi(sc_str+1);
        while (*sc_str && *sc_str != ' ' && *sc_str != '\t' && *sc_str != '=') sc_str++;
        if (*sc_str == '=')
        {
#ifdef WDL_HOOK_LOCALIZE_ATOF
          v = (float)WDL_HOOK_LOCALIZE_ATOF(sc_str+1);
#else
          v = (float)atof(sc_str+1);
#endif
          if (id > 0)
          {
            ctl_scale_info inf = { v };
            while (*sc_str && *sc_str != ' ' && *sc_str != '\t')
            {
              if (!strnicmp(sc_str,",dx=",4)) inf.xadj = (float)atof(sc_str+4);
              else if (!strnicmp(sc_str,",ysc=",5)) inf.ysc = (float)atof(sc_str+5);
              sc_str++;
            }
            ctl_scales.AddUnsorted(id,inf);
          }
        }
      }
      else if (!strnicmp(sc_str,"auto_expand",11)) auto_expand = true;
      while (*sc_str && *sc_str != ' ' && *sc_str != '\t') sc_str++;
    }
    ctl_scales.Resort();
  }

  windowReorgState s(hwnd,scx,scy);

  char buf[8192];
  xlateWindow(hwnd,sec,buf,sizeof(buf),false); // translate window title
  EnumChildWindows(hwnd,xlateGetRects,(LPARAM)&s);

#ifdef _WIN32
  HDC hdc=GetDC(hwnd);
  HGDIOBJ oldFont=0;
  HFONT font = (HFONT)SendMessage(hwnd,WM_GETFONT,0,0);
  if (font) oldFont=SelectObject(hdc,font);
#endif

  const bool do_columns = true;
  for (int x=0;x<s.cws.GetSize();x++)
  {
    windowReorgEnt *rec=s.cws.Get()+x;
    if (rec->hwnd)
    {
      const char *newText=xlateWindow(rec->hwnd,sec,buf,sizeof(buf), rec->mode != windowReorgEnt::WRET_MISC);
      const ctl_scale_info *this_sc_ptr = ctl_scales.GetPtr(rec->wnd_id);
      int dSize = 0;
      if (this_sc_ptr && this_sc_ptr->ysc > 1.0)
      {
        const int addh = (int)floor(this_sc_ptr->ysc * (rec->r.bottom-rec->r.top) + 0.5) - (rec->r.bottom - rec->r.top);
        if (addh > 0)
        {
          rec->r.top -= addh/2;
          rec->r.bottom += (addh+1)/2;
        }
      }

      int xadj = 0;
      if (this_sc_ptr && this_sc_ptr->xadj != 0.0)
      {
        xadj = (int) floor(this_sc_ptr->xadj * (rec->orig_r.right-rec->orig_r.left) + 0.5);
        rec->r.right += xadj;
        rec->r.left += xadj;
      }
      if (this_sc_ptr && this_sc_ptr->xsc != 1.0 && this_sc_ptr->xsc > 0.1 && this_sc_ptr->xsc < 8.0)
      {
        RECT r1;
        GetClientRect(rec->hwnd,&r1);
        dSize = (int) floor(r1.right * this_sc_ptr->xsc + 0.5) - r1.right;
        if (dSize > 0 && xadj < 0)
        {
          const int amt = wdl_min(-xadj,dSize);
          rec->r.right += amt;
          dSize -= amt;
        }
        if (dSize!=0)
          rec->mode = windowReorgEnt::WRET_SIZEADJ;
      }
      else
      {
        if (newText && rec->mode == windowReorgEnt::WRET_SIZEADJ)
        {
          RECT r1={0},r2={0};
#ifdef _WIN32
          DrawTextUTF8(hdc,buf,-1,&r1,DT_CALCRECT);
          DrawTextUTF8(hdc,newText,-1,&r2,DT_CALCRECT);
          r1.right += rec->scaled_width_change;
#else
          GetClientRect(rec->hwnd,&r1);
          SWELL_GetDesiredControlSize(rec->hwnd,&r2);
#endif
          if (r2.right > r1.right)
          {
            if (this_sc_ptr && this_sc_ptr->xadj < 0.0)
            {
              // if specified no xsc, but a negative dx, then we should grow to the left
              rec->r.left -= r2.right-r1.right;
            }
            else
              dSize=r2.right-r1.right;
          }
        }
      }

      if (dSize!=0)
      {
        if (dSize>0) dSize++;
        rec->wantsizeincrease = dSize;

        if (do_columns && dSize>0)
        {
          const int v = (rec->r.right<<16) | (rec->r.left&0xffff);
          int *diff = s.columns.GetPtr(v);
          if (!diff) s.columns.Insert(v,dSize);
          else if (dSize > *diff) *diff = dSize;
        }
      }
    }
  }

  if (do_columns) for (int x=0;x<s.cws.GetSize();x++)
  {
    windowReorgEnt *rec=s.cws.Get()+x;
    if (rec->hwnd && rec->mode == windowReorgEnt::WRET_SIZEADJ)
    {
      const int v = (rec->r.right<<16) | (rec->r.left&0xffff);
      const int *diff = s.columns.GetPtr(v);
      if (diff) rec->wantsizeincrease = *diff;
    }
  }

  int swSizeInc=0;

  do
  {
    qsort(s.cws.Get(),s.cws.GetSize(),sizeof(*s.cws.Get()),windowReorgEnt::Sort);
    int maxwant = 0;
    for (int x=0;x<s.cws.GetSize();x++)
    {
      windowReorgEnt *trec=s.cws.Get()+x;
      if (trec->wantsizeincrease<0)
      {
        trec->r.right += trec->wantsizeincrease;
        trec->wantsizeincrease = 0;
      }
      else if (trec->wantsizeincrease>0)
      {
        int amt = rippleControlsRight(trec->hwnd,&trec->r,trec+1,s.cws.GetSize() - (x+1),trec->wantsizeincrease,
            (auto_expand?2000:0)+s.par_cr.right);
        if (amt>0)
        {
          trec->wantsizeincrease -= amt;
          trec->r.right += amt;
        }
        for (int y=x+1;y<s.cws.GetSize();y++)
        {
          windowReorgEnt *rec=s.cws.Get()+y;
          int a = min(amt,rec->move_amt);
          if (a>0)
          {
            rec->r.left += a;
            rec->r.right += a;
          }
          rec->move_amt=0;
        }
        if (!swSizeInc && trec->wantsizeincrease>0) swSizeInc=1;
      }
      maxwant = wdl_max(maxwant,trec->r.right - s.par_cr.right);
    }
    if (!auto_expand && swSizeInc++)
    {
      // langpack did not specify auto_expand, everything didn't fit, flip and try again (and flip back)
      int w=s.par_cr.right;
      for (int x=0;x<s.cws.GetSize();x++)
      {
        windowReorgEnt *rec=s.cws.Get()+x;
        int a = w-1-rec->r.left;
        int b = w-1-rec->r.right;
        rec->r.left = b;
        rec->r.right = a;
      }
    }
    if (maxwant > 0 && auto_expand)
    {
      // langpack specified auto_expand: expand window up
      RECT wr;
      if (GetWindowLong(hwnd,GWL_STYLE)&WS_CHILD) wr=s.par_cr;
      else GetWindowRect(hwnd,&wr);
      s.par_cr.right += maxwant;
      SetWindowPos(hwnd,NULL,0,0, maxwant + (wr.right-wr.left),
                              (wr.bottom-wr.top),
                              SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);
    }
  } while (swSizeInc == 2);

  for (int x=0;x<s.cws.GetSize();x++)
  {
    windowReorgEnt *rec=s.cws.Get()+x;
    if (rec->hwnd)
    {
      bool wantMove = rec->r.left != rec->orig_r.left || rec->r.top != rec->orig_r.top;
      bool wantSize = (rec->r.right-rec->r.left) != (rec->orig_r.right-rec->orig_r.left) ||
                      (rec->r.bottom-rec->r.top) != (rec->orig_r.bottom-rec->orig_r.top) ;
      if (wantMove||wantSize)
      {
        SetWindowPos(rec->hwnd,NULL,rec->r.left,rec->r.top,rec->r.right-rec->r.left,rec->r.bottom-rec->r.top,
          (wantSize?0:SWP_NOSIZE)|(wantMove?0:SWP_NOMOVE)|SWP_NOZORDER|SWP_NOACTIVATE);
      }
    }
  }
#ifdef _WIN32
  if (oldFont) SelectObject(hdc,oldFont);
  ReleaseDC(hwnd,hdc);
#endif
}

void __localizeInitializeDialog(HWND hwnd, const char *desc)
{
  if (!desc || !hwnd || !*desc) return;
  WDL_KeyedArray<WDL_UINT64, char *> *s = g_translations.Get(desc);
  if (s) localize_dialog(hwnd,s);
}

void (*localizePreInitDialogHook)(HWND hwndDlg);

static WDL_DLGRET __localDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (uMsg)
  {
    case WM_INITDIALOG:
      {
        void **l = (void **)lParam;

        if (localizePreInitDialogHook)
          localizePreInitDialogHook(hwnd);

        if (l[2])
          localize_dialog(hwnd,(WDL_KeyedArray<WDL_UINT64, char *> *)l[2]);

#ifdef _WIN32
        {
          // if not a child window and has a menu set, localize our menu
          WDL_KeyedArray<WDL_UINT64, char *> *s = (WDL_KeyedArray<WDL_UINT64, char *> *)l[3];
          if (s && !(GetWindowLong(hwnd,GWL_STYLE)&WS_CHILD))
          {
            HMENU hmenu = GetMenu(hwnd);
            if (hmenu) __localProcMenu(hmenu,s);
          }
        }
#endif

        DLGPROC newproc = (DLGPROC) l[0];
        SetWindowLongPtr(hwnd,DWLP_DLGPROC,(LRESULT) newproc);

        return newproc(hwnd,uMsg,wParam,(LPARAM)l[1]);
      }
  }
  return 0;
}

#ifdef _WIN32
static WORD __getMenuIdFromDlgResource(HINSTANCE hInstance, const char * lpTemplate)
{
  HRSRC h=FindResource(hInstance,lpTemplate,RT_DIALOG);
  if (!h) return 0;
  DWORD sz = SizeofResource(hInstance, h);
  if (sz<30) return 0;

  HGLOBAL hg=LoadResource(hInstance,h);
  if (!hg) return 0;
  WORD *p = (WORD *)LockResource(hg);
  if (!p) return 0;

  if (p[0]!=1 || p[1] != 0xffff) return 0; // not extended dialog template
  if (p[13]==0) return 0;
  if (p[13]==0xffff) return p[14];

  //otherwise it is a string, but we dont support(or use) that
  return 0;

}
#endif

DLGPROC __localizePrepareDialog(const char *rescat, HINSTANCE hInstance, const char *lpTemplate, DLGPROC dlgProc, LPARAM lParam, void **ptrs, int nptrs)
{
  INT_PTR a = (INT_PTR) lpTemplate;
  if (WDL_NOT_NORMALLY(nptrs<4)) return NULL;

  WDL_KeyedArray<WDL_UINT64, char *> *s = NULL, *s2 = NULL;
  if (a>0&&a<65536)
  {
    char buf[128];
    snprintf(buf,sizeof(buf),"%sDLG_%d",rescat?rescat:"",(int)a);
    s = g_translations.Get(buf);
#ifdef _WIN32
    int menuid = __getMenuIdFromDlgResource(hInstance,lpTemplate);
    if (menuid)
    {
      snprintf(buf,sizeof(buf),"%sMENU_%d",rescat?rescat:"",menuid);
      s2=g_translations.Get(buf);
    }
#endif
  }

  ptrs[0] = (void*)dlgProc;
  ptrs[1] = (void*)(INT_PTR)lParam;
  ptrs[2] = s;
  ptrs[3] = s2;
#ifdef WDL_LOCALIZE_HOOK_DLGPROC
  WDL_LOCALIZE_HOOK_DLGPROC
#endif
  return (s||s2||(a>0 && localizePreInitDialogHook)) ? __localDlgProc : NULL;
}

HWND __localizeDialog(HINSTANCE hInstance, const char *lpTemplate, HWND hwndParent, DLGPROC dlgProc, LPARAM lParam, int mode)
{
  void *p[5];
  char tmp[256];
  if (mode == 1)
  {
    p[4] = tmp;
    snprintf(tmp,sizeof(tmp),"DLG%d",(int)(INT_PTR)lpTemplate);
  }
  else
    p[4] = NULL;
  DLGPROC newDlg  = __localizePrepareDialog(NULL,hInstance,lpTemplate,dlgProc,lParam,p,sizeof(p)/sizeof(p[0]));
  if (newDlg)
  {
    dlgProc = newDlg;
    lParam = (LPARAM)(INT_PTR)p;
  }
  switch (mode)
  {
    case 0: return CreateDialogParam(hInstance,lpTemplate,hwndParent,dlgProc,lParam);
    case 1: return (HWND) (INT_PTR)DialogBoxParam(hInstance,lpTemplate,hwndParent,dlgProc,lParam);
  }
  return 0;
}

// call at start of file with format_flag=-1
// expect possible blank (\r or \n) lines, too, ignore them
// format flag will get set to: 0=utf8, 1=utf16le, 2=utf16be, 3=ansi
void WDL_fgets_as_utf8(char *linebuf, int linebuf_size, FILE *fp, int *format_flag)
{
  linebuf[0]=0;
  if (*format_flag>0)
  {
    int sz=0;
    while (sz < linebuf_size-8)
    {
      int a = fgetc(fp);
      int b = *format_flag==3 ? 0 : fgetc(fp);
      if (a<0 || b<0) break;
      if (*format_flag==2) a = (a<<8)+b;
      else a += b<<8;


    again:
      if (a >= 0xD800 && a < 0xDC00) // UTF-16 supplementary planes
      {
        int aa = fgetc(fp);
        int bb = fgetc(fp);
        if (aa < 0 || bb < 0) break;

        if (*format_flag==2) aa = (aa<<8)+bb;
        else aa += bb<<8;

        if (aa >= 0xDC00 && aa < 0xE000)
        {
          a = 0x10000 + ((a&0x3FF)<<10) + (aa&0x3FF);
        }
        else
        {
          a=aa;
          goto again;
        }
      }


      if (a < 0x80) linebuf[sz++] = a;
      else
      {
        if (a<0x800)  // 11 bits (5+6 bits)
        {
          linebuf[sz++] = 0xC0 + ((a>>6)&31);
        }
        else
        {
          if (a < 0x10000) // 16 bits (4+6+6 bits)
          {
            linebuf[sz++] = 0xE0 + ((a>>12)&15); // 4 bits
          }
          else // 21 bits yow
          {
            linebuf[sz++] = 0xF0 + ((a>>18)&7); // 3 bits
            linebuf[sz++] = 0x80 + ((a>>12)&63); // 6 bits
          }
          linebuf[sz++] = 0x80 + ((a>>6)&63); // 6 bits
        }
        linebuf[sz++] = 0x80 + (a&63); // 6 bits
      }

      if (a == '\n') break;
    }
    linebuf[sz]=0;
  }
  else
  {
    fgets(linebuf,linebuf_size,fp);
  }

  if (linebuf[0] && *format_flag<0)
  {
    unsigned char *p=(unsigned char *)linebuf;
    if (p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBf)
    {
      *format_flag=0;
      memmove(linebuf,linebuf+3,strlen(linebuf+3)+1); // skip UTF-8 BOM
    }
    else if ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))
    {
      fseek(fp,2,SEEK_SET);
      *format_flag=p[0] == 0xFE ? 2 : 1;
      WDL_fgets_as_utf8(linebuf,linebuf_size,fp,format_flag);
      return;
    }
    else
    {
      for (;;)
      {
        const unsigned char *str=(unsigned char *)linebuf;
        while (*str)
        {
          unsigned char c = *str++;
          if (c >= 0xC0)
          {
            if (c <= 0xDF && str[0] >=0x80 && str[0] <= 0xBF) str++;
            else if (c <= 0xEF && str[0] >=0x80 && str[0] <= 0xBF && str[1] >=0x80 && str[1] <= 0xBF) str+=2;
            else if (c <= 0xF7 &&
                     str[0] >=0x80 && str[0] <= 0xBF &&
                     str[1] >=0x80 && str[1] <= 0xBF &&
                     str[2] >=0x80 && str[2] <= 0xBF) str+=3;
            else break;
          }
          else if (c >= 128) break;
        }
        if (*str) break;

        linebuf[0]=0;
        fgets(linebuf,linebuf_size,fp);
        if (!linebuf[0]) break;
      }
      *format_flag = linebuf[0] ? 3 : 0; // if scanned the whole file without an invalid UTF8 pair, then UTF-8 (0), otherwise ansi (3)
      fseek(fp,0,SEEK_SET);
      WDL_fgets_as_utf8(linebuf,linebuf_size,fp,format_flag);
      return;
    }
  }
}

WDL_KeyedArray<WDL_UINT64, char *> *WDL_GetLangpackSection(const char *sec)
{
  return g_translations.Get(sec);
}

WDL_KeyedArray<WDL_UINT64, char *> *WDL_LoadLanguagePackInternal(const char *fn,
    WDL_StringKeyedArray< WDL_KeyedArray<WDL_UINT64, char *> * > *dest,
    const char *onlySec_name,
    bool include_commented_lines,
    bool no_escape_strings,
    WDL_StringKeyedArray<char *> *extra_metadata
    )
{
#ifdef _DEBUG
  g_debug_langpack_has_loaded = true;
#endif
  WDL_KeyedArray<WDL_UINT64, char *> *rv=NULL;
  FILE *fp = fopenUTF8(fn,"r");
  if (!fp) return rv;

  WDL_KeyedArray<WDL_UINT64, char *> *cursec=NULL;

  int format_flag=-1;

  WDL_TypedBuf<char> procbuf;
  char linebuf[16384];
  int ic_lines = 0;
  for (;;)
  {
    WDL_fgets_as_utf8(linebuf,sizeof(linebuf),fp,&format_flag);
    if (!linebuf[0]) break;

    char *p=linebuf;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    char *lbstart = p;
    while (*p) p++;
    p--;
    while (p >= lbstart && (*p == '\t' || *p == '\n' || *p == '\r')) p--;
    p++;
    *p=0;

    if (include_commented_lines)
    {
      if (*lbstart == ';')
      {
        int x, offs = (lbstart[1] == '^') ? 2 : 1;
        for (x = 0; x < 16; x ++)
        {
          char c = lbstart[offs+x];
          if (c >= 'A' && c <= 'F') { }
          else if (c >= '0' && c <= '9') { }
          else break;
        }
        if (x == 16 && lbstart[offs+16] == '=')
          lbstart += offs;
      }
    }
    if (!*lbstart || *lbstart == ';' || *lbstart == '#')
    {
      if (ic_lines >= 0 && extra_metadata)
      {
        char tmp[128];
        snprintf(tmp,sizeof(tmp),"_initial_comment_%d",ic_lines++);
        extra_metadata->Insert(tmp,strdup(linebuf));
      }
      continue;
    }

    if (*lbstart == '[')
    {
      ic_lines = -1;
      if (cursec) cursec->Resort();

      lbstart++;
      {
        char *tmp = lbstart;
        while (*tmp && *tmp != ']') tmp++;
        *tmp++=0;
        if (extra_metadata)
        {
          while (*tmp == ' ') tmp++;
          if (*tmp)
            extra_metadata->Insert(lbstart,strdup(tmp));
        }
      }

      if (onlySec_name)
      {
        cursec=NULL;
        if (!strcmp(lbstart,onlySec_name))
        {
          if (!rv) rv=new WDL_KeyedArray<WDL_UINT64, char *>;
          cursec=rv;
        }
      }
      else
      {
        cursec = dest->Get(lbstart);
        if (!cursec)
        {
          cursec = new WDL_KeyedArray<WDL_UINT64, char *>;
          dest->Insert(lbstart,cursec);
        }
      }
    }
    else if (cursec)
    {
      char *eq = strstr(lbstart,"=");
      if (eq)
      {
        *eq++ = 0;
        if (strlen(lbstart) == 16)
        {
          WDL_UINT64 v=0;
          int x;
          for (x=0;x<16;x++)
          {
            int a = lbstart[x];
            if (a>='0' && a<='9') a-='0';
            else if (a>='A' && a<='F') a-='A' - 10;
            else break;

            v<<=4;
            v+=a;
          }
          if (x==16)
          {
            if (strstr(eq,"\\") && !no_escape_strings)
            {
              procbuf.Resize(0,false);
              while (*eq)
              {
                if (*eq == '\\')
                {
                  eq++;
                  if (*eq == '\\' || *eq == '\'' || *eq == '"') procbuf.Add(*eq);
                  else if (*eq == 't'||*eq=='T') procbuf.Add('\t');
                  else if (*eq == 'r'||*eq=='R') procbuf.Add('\r');
                  else if (*eq == 'n'||*eq=='N') procbuf.Add('\n');
                  else if (*eq == '0') procbuf.Add(0);
                  else procbuf.Add(*eq);
                }
                else procbuf.Add(*eq);

                eq++;
              }
              procbuf.Add(0);
              procbuf.Add(0);
              char *pc = (char *)ChunkAlloc(procbuf.GetSize() + WDL_LOCALIZE_REC_HEADER_SIZE);
              if (pc)
              {
                memset(pc,0,WDL_LOCALIZE_REC_HEADER_SIZE);
                pc += WDL_LOCALIZE_REC_HEADER_SIZE;
                memcpy(pc,procbuf.Get(),procbuf.GetSize());
                cursec->AddUnsorted(v,pc);
              }
            }
            else
            {
              int eqlen = (int)strlen(eq);
              char *pc = (char *)ChunkAlloc(eqlen+2 + WDL_LOCALIZE_REC_HEADER_SIZE);
              if (pc)
              {
                memset(pc,0,WDL_LOCALIZE_REC_HEADER_SIZE);
                pc += WDL_LOCALIZE_REC_HEADER_SIZE;
                memcpy(pc,eq,eqlen+1);
                pc[eqlen+1]=0; // doublenull terminate to be safe, in case the caller requested LOCALIZE_FLAG_NULLPAIR
                cursec->AddUnsorted(v,pc);
              }
            }
          }
        }
      }
    }
  }
  if (cursec)
    cursec->Resort();

  fclose(fp);

  return rv;
}

WDL_KeyedArray<WDL_UINT64, char *> *WDL_LoadLanguagePack(const char *fn, const char *onlySec_name)
{
  WDL_KeyedArray<WDL_UINT64, char *> *rv = WDL_LoadLanguagePackInternal(fn,&g_translations, onlySec_name,false,false, NULL);
  if (!onlySec_name)
    g_translations_commonsec = g_translations.Get("common");
  return rv;
}

void WDL_SetLangpackFallbackEntry(const char *src_sec, WDL_UINT64 src_v, const char *dest_sec, WDL_UINT64 dest_v)
{
  WDL_KeyedArray<WDL_UINT64, char *> *sec = g_translations.Get(src_sec);
  char *v = sec ? sec->Get(src_v) : NULL;
  if (!v) return;
  sec = g_translations.Get(dest_sec);
  if (!sec)
  {
    sec = new WDL_KeyedArray<WDL_UINT64, char *>;
    g_translations.Insert(dest_sec,sec);
  }
  else if (sec->Get(dest_v)) return;

  sec->Insert(dest_v,v);
}


