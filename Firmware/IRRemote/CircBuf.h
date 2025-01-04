// Pinscape Pico firmware - IRQ-safe Circular Buffer
// Copyright 2017, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Circular buffer for incoming IRQ data.  We write reports in the IRQ
// handler, and we read reports in normal thread context (non-IRQ).
// 
// The design is organically safe for IRQ threading; there are no
// critical sections.  The IRQ context has exclusive write access to the
// write pointer, and the application context has exclusive write access
// to the read pointer, so there are no test-and-set or read-and-modify
// race conditions.
//
// The buffer can be used in the opposite direction as well, to send
// data from application context to IRQ context.  As in the reverse
// situation, one side has exclusive write access to one pointer, and
// the other context has exclusive write access to the other pointer, so
// there are no read/modify race conditions.

#pragma once
#include <stdlib.h>
#include <stdint.h>


// Circular buffer with a fixed buffer size
template<class T, int cnt> class CircBuf
{
public:
    CircBuf() { }

    // Read an item from the buffer.  Returns true if an item was available,
    // false if the buffer was empty.
    bool Read(T &result) 
    {
        if (iRead != iWrite)
        {
            memcpy(&result, &buf[iRead], sizeof(T));
            iRead = Advance(iRead);
            return true;
        }
        else
            return false;
    }
    
    // is an item ready to read?
    bool IsReadReady() const { return iRead != iWrite; }
    
    // Write an item to the buffer.  (Called in the IRQ handler, in interrupt
    // context.)
    bool Write(const T &item)
    {
        int nxt = Advance(iWrite);
        if (nxt != iRead)
        {
            memcpy(&buf[iWrite], &item, sizeof(T));
            iWrite = nxt;
            return true;
        }
        else
            return false;
    }
    
private:
    int Advance(int i)
    {
        ++i;
        return i < cnt ? i : 0;
    } 

    // buffer
    T buf[cnt];

    // read/write indices
    volatile int iRead = 0;
    volatile int iWrite = 0;
};

// Circular buffer with a variable buffer size
template<class T> class CircBufV
{
public:
    CircBufV() : buf(nullptr), cnt(0) { }
    CircBufV(int cnt) : buf(new T[cnt]), cnt(cnt) { }

    // allocate/reallocate
    void Alloc(int cnt)
    {
        delete[] buf;
        buf = new T[cnt];
        this->cnt = cnt;
        iRead = iWrite = 0;
    }
    
    ~CircBufV()
    {
        delete[] buf;
    }

    // Read an item from the buffer.  Returns true if an item was available,
    // false if the buffer was empty.  (Called in the main loop, in application
    // context.)
    bool Read(T &result) 
    {
        if (iRead != iWrite)
        {
            //{uint8_t *d = buf[iRead].data; printf("circ read [%02x %02x %02x %02x %02x %02x %02x %02x]\r\n", d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7]);}
            memcpy(&result, &buf[iRead], sizeof(T));
            iRead = Advance(iRead);
            return true;
        }
        else
            return false;
    }
    
    // is an item ready to read?
    bool IsReadReady() const { return iRead != iWrite; }
    
    // Write an item to the buffer.  (Called in the IRQ handler, in interrupt
    // context.)
    bool Write(const T &item)
    {
        int nxt = Advance(iWrite);
        if (nxt != iRead)
        {
            memcpy(&buf[iWrite], &item, sizeof(T));
            iWrite = nxt;
            return true;
        }
        else
            return false;
    }

private:
    int Advance(int i)
    {
        ++i;
        return i < cnt ? i : 0;
    } 

    // buffer
    T *buf;
    int cnt;

    // read/write indices
    int iRead = 0;
    int iWrite = 0;
};

