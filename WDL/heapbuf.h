/*
    WDL - heapbuf.h
    Copyright (C) 2005 and later Cockos Incorporated

    This software is provided 'as-is', without any express or implied
    warranty.  In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
       claim that you wrote the original software. If you use this software
       in a product, an acknowledgment in the product documentation would be
       appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be
       misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.
      
*/

/*

  This file provides the interface and implementation for WDL_HeapBuf, a simple 
  malloc() wrapper for resizeable blocks.

  Also in this file is WDL_TypedBuf which is a templated version WDL_HeapBuf 
  that manages type and type-size.
 
*/

#ifndef _WDL_HEAPBUF_H_
#define _WDL_HEAPBUF_H_

#ifndef WDL_HEAPBUF_IMPL_ONLY

#include "wdltypes.h"

class WDL_HeapBuf
{
  public:
    // interface 
#ifdef WDL_HEAPBUF_INTF_ONLY
    void *Resize(int newsize, bool resizedown=true);
    void CopyFrom(const WDL_HeapBuf *hb, bool exactCopyOfConfig=false);
    void SwapContentsWith(WDL_HeapBuf *hb); // swaps members, avoids memcpy() of underlying data
#endif
    void *Get() const { return m_size?m_buf:NULL; } // returns NULL if size is 0
    void *GetFast() const { return m_buf; } // returns last buffer if size is 0
    int GetSize() const { return m_size; }
    void *GetAligned(int align) const {  return (void *)(((UINT_PTR)Get() + (align-1)) & ~(UINT_PTR)(align-1)); }

    void SetGranul(int granul) { m_granul = granul; }
    int GetGranul() const { return m_granul; }
    void Prealloc(int sz) { if (m_alloc < sz && WDL_NORMALLY(m_size < sz)) { const int oldsz = m_size; Resize(sz,false); m_size=oldsz; } }
    int GetAlloc() const { return m_alloc; }

    void *ResizeOK(int newsize, bool resizedown = true) { void *p=Resize(newsize, resizedown); return GetSize() == newsize ? p : NULL; }
    
    WDL_HeapBuf(const WDL_HeapBuf &cp)
    {
      m_buf=0;
      CopyFrom(&cp,true);
    }
    WDL_HeapBuf &operator=(const WDL_HeapBuf &cp)
    {
      CopyFrom(&cp,false);
      return *this;
    }

    void ResizeToCurrent()
    {
      if (!m_buf || !m_size)
      {
        free(m_buf);
        m_buf = NULL;
        m_alloc = 0;
      }
      else if (m_alloc != m_size)
      {
        void *nb = realloc(m_buf,m_size);
        if (WDL_NORMALLY(nb!=NULL))
        {
          m_buf = nb;
          m_alloc = m_size;
        }
      }
    }

    explicit WDL_HeapBuf(int granul=4096) : m_buf(NULL), m_alloc(0), m_size(0), m_granul(granul)
    {
    }
    ~WDL_HeapBuf()
    {
#ifdef WDL_HEAPBUF__TRACE_ACTION
      if (m_buf) WDL_HEAPBUF__TRACE_ACTION(heapbuf_delete_free);
#endif
      free(m_buf);
    }

#endif // !WDL_HEAPBUF_IMPL_ONLY

    // implementation bits
#ifndef WDL_HEAPBUF_INTF_ONLY
    #ifdef WDL_HEAPBUF_IMPL_ONLY
      void *WDL_HeapBuf::Resize(int newsize, bool resizedown)
    #else
      void *Resize(int newsize, bool resizedown=true)
    #endif
      {
        if (newsize<0) newsize=0;
        #ifdef DEBUG_TIGHT_ALLOC // horribly slow, do not use for release builds
          if (newsize == m_size) return m_buf;

          int a = newsize; 
          if (a > m_size) a=m_size;
          void *newbuf = newsize ? malloc(newsize) : 0;
          if (!newbuf && newsize) 
          {
            #ifdef WDL_HEAPBUF_ONMALLOCFAIL
                WDL_HEAPBUF_ONMALLOCFAIL(newsize)
            #endif
            return m_buf;
          }
          if (newbuf&&m_buf) memcpy(newbuf,m_buf,a);
          m_size=m_alloc=newsize;
          free(m_buf);
          return m_buf=newbuf;
        #endif

          if (newsize!=m_size || (resizedown && newsize < m_alloc/2))
          {
            int resizedown_under = 0;
            if (resizedown && newsize < m_size)
            {
              // shrinking buffer: only shrink if allocation decreases to min(alloc/2, alloc-granul*4) or 0
              resizedown_under = m_alloc - (m_granul<<2);
              if (resizedown_under > m_alloc/2) resizedown_under = m_alloc/2;
              if (resizedown_under < 1) resizedown_under=1;
            }
  
            if (newsize > m_alloc || newsize < resizedown_under)
            {
              int granul=newsize/2;
              int newalloc;
              if (granul < m_granul) granul=m_granul;
  
              if (newsize<1) newalloc=0;
              else if (m_granul<4096) newalloc=newsize+granul;
              else
              {
                granul &= ~4095;
                if (granul< 4096) granul=4096;
                else if (granul>4*1024*1024) granul=4*1024*1024;
                newalloc = ((newsize + granul + 96)&~4095)-96;
              }
         
              if (newalloc != m_alloc)
              {

                if (newalloc <= 0)
                {
#ifdef WDL_HEAPBUF__TRACE_ACTION
                 if (m_buf) WDL_HEAPBUF__TRACE_ACTION(heapbuf_free);
#endif
                  free(m_buf);
                  m_buf=0;
                  m_alloc=0;
                  m_size=0;
                  return 0;
                }
#ifdef WDL_HEAPBUF__TRACE_ACTION
                if (m_buf) WDL_HEAPBUF__TRACE_ACTION(heapbuf_realloc);
                else WDL_HEAPBUF__TRACE_ACTION(heapbuf_malloc);
#endif
                void *nbuf=realloc(m_buf,newalloc);
                if (!nbuf)
                {
#ifdef WDL_HEAPBUF__TRACE_ACTION
                  WDL_HEAPBUF__TRACE_ACTION(heapbuf_mallocfree);
#endif
                  if (!(nbuf=malloc(newalloc))) 
                  {
                    #ifdef WDL_HEAPBUF_ONMALLOCFAIL
                      WDL_HEAPBUF_ONMALLOCFAIL(newalloc);
                    #endif
                    return m_size?m_buf:0; // failed, do not resize
                  }

                  if (m_buf) 
                  {
                    int sz=newsize<m_size?newsize:m_size;
                    if (sz>0) memcpy(nbuf,m_buf,sz);
                    free(m_buf);
                  }
                }
  
                m_buf=nbuf;
                m_alloc=newalloc;
              } // alloc size change
            } // need size up or down
            m_size=newsize;
          } // size change
          return m_size?m_buf:0;
      }

    #ifdef WDL_HEAPBUF_IMPL_ONLY
      void WDL_HeapBuf::CopyFrom(const WDL_HeapBuf *hb, bool exactCopyOfConfig)
    #else
      void CopyFrom(const WDL_HeapBuf *hb, bool exactCopyOfConfig=false)
    #endif
      {
        if (exactCopyOfConfig) // copy all settings
        {
          free(m_buf);

          m_granul = hb->m_granul;

          m_size=m_alloc=0;
          m_buf=hb->m_buf && hb->m_alloc>0 ? malloc(m_alloc = hb->m_alloc) : NULL;
          #ifdef WDL_HEAPBUF_ONMALLOCFAIL
            if (!m_buf && m_alloc) { WDL_HEAPBUF_ONMALLOCFAIL(m_alloc) } ;
          #endif
          if (m_buf) memcpy(m_buf,hb->m_buf,m_size = hb->m_size);
          else m_alloc=0;
        }
        else // copy just the data + size
        {
          const int newsz=hb->GetSize();
          Resize(newsz,true);
          if (GetSize()!=newsz) Resize(0);
          else memcpy(Get(),hb->Get(),newsz);
        }
      }
    #ifdef WDL_HEAPBUF_IMPL_ONLY
      void WDL_HeapBuf::SwapContentsWith(WDL_HeapBuf *hb) // swaps members, avoids memcpy() of underlying data
    #else
      void SwapContentsWith(WDL_HeapBuf *hb)
    #endif
      {
        // does not swap granul
        if (WDL_NOT_NORMALLY(!hb || hb == this)) return;
        void * const b = hb->m_buf;
        const int a = hb->m_alloc, sz = hb->m_size;
        hb->m_buf = m_buf;
        hb->m_alloc = m_alloc;
        hb->m_size = m_size;
        m_buf = b;
        m_alloc = a;
        m_size = sz;
      }

#endif // ! WDL_HEAPBUF_INTF_ONLY

#ifndef WDL_HEAPBUF_IMPL_ONLY

  private:
    void *m_buf;
    int m_alloc;
    int m_size;
    int m_granul;

  #if defined(_WIN64) || defined(__LP64__)
  public:
    int ___pad; // keep size 8 byte aligned
  #endif

};

template<class PTRTYPE> class WDL_TypedBuf 
{
  public:
    PTRTYPE *Get() const { return (PTRTYPE *) m_hb.Get(); }
    PTRTYPE *GetFast() const { return (PTRTYPE *) m_hb.GetFast(); }
    int GetSize() const { return m_hb.GetSize()/(unsigned int)sizeof(PTRTYPE); }
    int GetSizeBytes() const { return m_hb.GetSize(); }
    int GetAlloc() const { return m_hb.GetAlloc()/(unsigned int)sizeof(PTRTYPE); }

    PTRTYPE *Resize(int newsize, bool resizedown = true) { return (PTRTYPE *)m_hb.Resize(newsize*sizeof(PTRTYPE),resizedown); }
    PTRTYPE *ResizeOK(int newsize, bool resizedown = true) { return (PTRTYPE *)m_hb.ResizeOK(newsize*sizeof(PTRTYPE), resizedown);  }

    void Prealloc(int sz) { return m_hb.Prealloc(sz*sizeof(PTRTYPE)); }
    void ResizeToCurrent() { m_hb.ResizeToCurrent(); }

    void SetToZero() { memset(m_hb.Get(), 0, m_hb.GetSize()); }

    PTRTYPE *GetAligned(int align) const  { return (PTRTYPE *) m_hb.GetAligned(align); }

    PTRTYPE *Add(PTRTYPE val) 
    {
      const int sz=GetSize();
      PTRTYPE* p=ResizeOK(sz+1,false);
      if (p)
      {
        p[sz]=val;
        return p+sz;
      }
      return NULL;
    }
    PTRTYPE *Add(const PTRTYPE *buf, int bufsz) 
    {
      if (bufsz>0)
      {
        const int sz=GetSize();
        PTRTYPE* p=ResizeOK(sz+bufsz,false);
        if (p)
        {
          p+=sz;
          if (buf) memcpy(p,buf,bufsz*sizeof(PTRTYPE));
          else memset((char*)p,0,bufsz*sizeof(PTRTYPE));
          return p;
        }
      }
      return NULL;
    }
    PTRTYPE *Set(const PTRTYPE *buf, int bufsz) 
    {
      if (bufsz>=0)
      {
        PTRTYPE* p=ResizeOK(bufsz,false);
        if (p)
        {
          if (buf) memcpy(p,buf,bufsz*sizeof(PTRTYPE));
          else memset((char*)p,0,bufsz*sizeof(PTRTYPE));
          return p;
        }
      }
      return NULL;
    }
    PTRTYPE* Insert(PTRTYPE val, int idx)
    {
      const int sz=GetSize();
      if (idx >= 0 && idx <= sz)
      {
        PTRTYPE* p=ResizeOK(sz+1,false);
        if (p)
        {
          memmove(p+idx+1, p+idx, (sz-idx)*sizeof(PTRTYPE));
          p[idx]=val;
          return p+idx;
        }
      }
      return NULL;
    }

    void Delete(int idx)
    {
      PTRTYPE* p=Get();
      const int sz=GetSize();
      if (idx >= 0 && idx < sz)
      {
        memmove(p+idx, p+idx+1, (sz-idx-1)*sizeof(PTRTYPE));
        Resize(sz-1,false);
      }
    }

    void DeleteRange(int index, int count)
    {
      PTRTYPE *list=Get();
      int size=GetSize();
      if (list && count > 0 && index >= 0 && index < size)
      {
        if (count > size - index) count = size - index;
        size -= count;
        if (index < size)
          memmove(list+index,list+index+count,(unsigned int)sizeof(PTRTYPE)*(size-index));
        Resize(size,false);
      }
    }

    void SetGranul(int gran) { m_hb.SetGranul(gran); }

    int Find(PTRTYPE val) const
    {
      const PTRTYPE* p=Get();
      const int sz=GetSize();
      int i;
      for (i=0; i < sz; ++i) if (p[i] == val) return i;
      return -1;
    }

    explicit WDL_TypedBuf(int granul=4096) : m_hb(granul) { }
    ~WDL_TypedBuf() { }

    WDL_HeapBuf *GetHeapBuf() { return &m_hb; }
    const WDL_HeapBuf *GetHeapBuf() const { return &m_hb; }

    int DeleteBatch(bool (*proc)(PTRTYPE *p, void *ctx), void *ctx=NULL) // proc returns true to delete item. returns number deleted
    {
      const int sz = GetSize();
      int cnt=0;
      PTRTYPE *rd = Get(), *wr = rd;
      for (int x = 0; x < sz; x ++)
      {
        if (!proc(rd,ctx))
        {
          if (rd != wr) *wr=*rd;
          wr++;
          cnt++;
        }
        rd++;
      }
      if (cnt < sz) Resize(cnt,false);
      return sz - cnt;
    }

    void SwapContentsWith(WDL_TypedBuf<PTRTYPE> *b) { m_hb.SwapContentsWith(&b->m_hb); }

    const PTRTYPE *begin() const { return Get(); }
    const PTRTYPE *end() const { return Get() + GetSize(); }
    PTRTYPE *begin() { return Get(); }
    PTRTYPE *end() { return Get() + GetSize(); }

  private:
    WDL_HeapBuf m_hb;
};

#endif // ! WDL_HEAPBUF_IMPL_ONLY

#endif // _WDL_HEAPBUF_H_
