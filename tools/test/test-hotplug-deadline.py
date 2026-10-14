#!/usr/bin/env python3
"""Exercise the actual sysevent deadline function, including jiffies wrap."""
from pathlib import Path
import os, subprocess, tempfile
s=(Path(__file__).resolve().parents[2]/'carried/drm_kms/illumos/drm_kms_hotplug.c').read_text()
s=s[s.index('static void\nhotplug_queue'):s.index('\nstatic int\nhotplug_emit')]
pre=r'''
#include <assert.h>
#include <limits.h>
#include <stdio.h>
struct delayed_work { int unused; };
struct drm_hotplug { unsigned long next; struct delayed_work work; };
static unsigned long clock_value, step, scheduled;
static int pending, reads;
static void *system_wq;
static unsigned long ticks(void) { unsigned long n=clock_value; clock_value+=step; reads++; return n; }
#define jiffies ticks()
#define msecs_to_jiffies(n) ((unsigned long)(n))
#define time_before(a,b) ((long)((a)-(b)) < 0)
#define max(a,b) ((a)>(b)?(a):(b))
static int queue_delayed_work(void *q,struct delayed_work *w,unsigned long delay) { if(pending) return 0; pending=1; scheduled=delay; return 1; }
'''
post=r'''
static void check(unsigned long now,unsigned long next,unsigned long expected) {
 struct drm_hotplug hp={.next=next}; clock_value=now; step=2; reads=pending=0;
 hotplug_queue(&hp); assert(reads==1); assert(scheduled==expected);
 for(int i=0;i<100;i++) hotplug_queue(&hp);
 assert(scheduled==expected); /* a storm does not extend the deadline */
}
int main(void) {
 check(10,11,250); /* crossing the next tick cannot underflow */
 check(1000,2000,1000);
 check(2000,1000,250);
 check(ULONG_MAX-100,400,501); /* unsigned clock wrap */
 puts("PASS: deadline edge, rate floor, overdue, wrap, pending storm");
}
'''
with tempfile.TemporaryDirectory(prefix='hotplug-deadline-') as td:
 p=Path(td); (p/'test.c').write_text(pre+s+post)
 subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Wno-unused-parameter',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
