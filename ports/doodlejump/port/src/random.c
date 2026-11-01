/*
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/* Adapted from Apple Libc-594.1.4 stdlib/FreeBSD/random.c, default TYPE_3 only.
 * https://github.com/apple-oss-distributions/Libc/blob/Libc-594.1.4/stdlib/FreeBSD/random.c
 * State is explicit so a game replay can reproduce the original random stream. */
#include "random.h"
uint32_t dj_random(DJRandom *r) {
    r->state[r->front]+=r->state[r->rear];
    uint32_t value=r->state[r->front]>>1;
    if(++r->front==31)r->front=0;
    if(++r->rear==31)r->rear=0;
    return value;
}
void dj_srandom(DJRandom *r,uint32_t seed) {
    r->state[0]=seed;
    for(int i=1;i<31;i++) {
        int32_t x=(int32_t)r->state[i-1];
        if(!x)x=123459876;
        int32_t hi=x/127773,lo=x%127773;
        x=16807*lo-2836*hi;
        if(x<0)x+=0x7fffffff;
        r->state[i]=(uint32_t)x;
    }
    r->front=3;r->rear=0;
    for(int i=0;i<310;i++)dj_random(r);
}
