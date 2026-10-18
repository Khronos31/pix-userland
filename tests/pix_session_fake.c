#include "diagnostic.h"
#include "dt295_capture.h"
#include "channel.h"
#include "control.h"
#include "os.h"
#include "pixy.h"
#include "transport.h"
#include "usb_backend.h"
#include <stdlib.h>
#include <signal.h>
#include <string.h>

struct usb_backend { int failure; usb_sink sink; void *sink_user; };
static uint64_t clock_ms;
static usb_backend *active_usb;
static void *(*worker_fn)(void *);
static void *worker_arg;
static int worker_created;
static int control_step;

static void event(const char *name) { fprintf(stderr, "TRACE %s\n", name); }
static int fault(const char *name) {
    const char *p = getenv("PIX_FAULT");
    if (!p) return 0;
    size_t n = strlen(name);
    while (*p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && !memcmp(p, name, n)) return 1;
        if (!end) break;
        p = end + 1;
    }
    return 0;
}
static int code_for(const char *name) { const char *p = getenv(name); return p ? atoi(p) : 7; }
uint64_t now_ms(void) { return clock_ms; }
void sleep_ms(unsigned ms) { clock_ms += ms; }
void secure_clear(void *p, size_t n) { memset(p, 0, n); }
int os_output_prepare(int fd) { (void)fd; return 0; }
int os_output_is_terminal(int fd) { (void)fd; return 0; }
void os_output_restore(int fd, int state) { (void)fd; (void)state; }
int usb_selector_parse(const char *p, usb_selector *s) { (void)p; memset(s, 0, sizeof(*s)); return 0; }
int pix_channel_parse(const char *p, pixy_system d, pix_channel *c) { (void)p; c->system=d; c->channel=27; c->slot=-1; return 0; }
int pixy_frequency(pixy_system s, unsigned c, uint32_t *hz) { (void)s; (void)c; *hz=545142857; return 0; }
int pix_control_poll(pix_control *p, pixy_system system, pix_command *c) {
    (void)p;
    (void)system;
    if (fault("control-retune") && control_step++ == 0) { memset(c,0,sizeof(*c)); c->channel.system=PIXY_TERRESTRIAL; c->channel.channel=28; c->channel.slot=-1; return 1; }
    if (fault("control-quit")) { memset(c,0,sizeof(*c)); c->quit=1; return 1; }
    if (fault("control-eof") || fault("control-retune")) return 2;
    return 0;
}
int usb_open(usb_backend **out, const usb_selector *s, int fd, int detach) {
    fprintf(stderr,"TRACE usb_open kind=%d index=%u fd=%d detach=%d\n",s->kind,s->index,fd,detach);
    if (fault("open")) return 3;
    *out=calloc(1,sizeof(**out)); active_usb=*out; return *out ? 0 : 70;
}
void usb_diagnostics(usb_backend *u, int enabled) { (void)u; fprintf(stderr,"TRACE usb_diagnostics enabled=%d\n",enabled); }
int usb_failure_exit(const usb_backend *u) { return u ? u->failure : 70; }
pixy_io usb_io(usb_backend *u) { (void)u; pixy_io io={0}; return io; }
int usb_stream_start(usb_backend *u, usb_sink sink, void *user) {
    fprintf(stderr,"TRACE usb_stream_start sink=%d user=%d\n",sink != NULL,user != NULL); u->sink=sink; u->sink_user=user;
    if (fault("stream-start")) { u->failure=7; return -1; } return 0;
}
int usb_poll(usb_backend *u, unsigned ms) { clock_ms+=1000; fprintf(stderr,"TRACE usb_poll ms=%u\n",ms); if (fault("signal")) raise(SIGINT); if (fault("poll")) { u->failure=7; return -1; } return 0; }
int usb_stream_stop(usb_backend *u) {
    event("usb_stream_stop");
    if (u->sink) { uint8_t p[32769]={0x47}; size_t n=fault("queue-error")?sizeof(p):188; event("final_enqueue"); u->sink(u->sink_user,p,n); }
    if (fault("usb-stop")) { u->failure=7; return -1; } return 0;
}
void usb_close(usb_backend *u) { event("usb_close"); if (active_usb == u) active_usb=NULL; free(u); }
void pixy_init(pixy *d, pixy_io io) { memset(d,0,sizeof(*d)); d->io=io; event("pixy_init"); }
static int pix_fail(pixy *d, const char *op, int fallback) {
    char key[80]; snprintf(key,sizeof(key),"fail-%s",op);
    if (!fault(key)) return 0;
    event(op); d->failure_exit=code_for("PIX_DEVICE_EXIT"); if (fault("also-usb") && active_usb) active_usb->failure=7; snprintf(d->error,sizeof(d->error),"fake %s failure",op); return fallback;
}
int pixy_power(pixy *d, const uint8_t *p, size_t n) { (void)p; fprintf(stderr,"TRACE pixy_power context=%d size=%zu\n",d->failure_context,n); return pix_fail(d,"power",-1); }
int pixy_session(pixy *d, const uint8_t *p, const uint8_t *v) { (void)p;(void)v; fprintf(stderr,"TRACE pixy_session context=%d\n",d->failure_context); return pix_fail(d,"session",-1); }
int pixy_tuner_init(pixy *d) { fprintf(stderr,"TRACE pixy_tuner_init context=%d\n",d->failure_context); return pix_fail(d,"tuner",-1); }
int pixy_tune_slot(pixy *d, pixy_system s, uint32_t hz, int lna, int *tsid, int slot) { (void)tsid; fprintf(stderr,"TRACE pixy_tune_slot system=%d hz=%u lna=%d slot=%d\n",s,hz,lna,slot); return pix_fail(d,"tune",-1); }
int pixy_status(pixy *d, uint8_t *p, size_t *n) { fprintf(stderr,"TRACE pixy_status capacity=%zu\n",*n); if (fault("status") || fault("fail-status")) return pix_fail(d,"status",-1); memset(p,0,*n); p[3]=fault("card-absent")?0:1; *n=64; return 0; }
int pixy_fifo_clear(pixy *d) { event("pixy_fifo_clear"); return pix_fail(d,"fifo",-1); }
int pixy_start(pixy *d, uint8_t *context) { event("pixy_start"); memset(context,0,32); if (pix_fail(d,"start",-1)) return -1; d->streaming=1; return 0; }
int pixy_stop(pixy *d) { event("pixy_stop"); d->streaming=0; return pix_fail(d,"device-stop",-1); }
int pixy_certify(pixy *d) { event("pixy_certify"); return pix_fail(d,"certify",-1); }
int pixy_shutdown(pixy *d) { event("pixy_shutdown"); return pix_fail(d,"shutdown",-1); }
diagnostic *diag_open(const char *p) { (void)p; return (diagnostic *)1; }
int diag_before(diagnostic *d,const ts_decoder *a,const ts_check *b,const uint8_t *p,size_t n,uint64_t t,unsigned q,unsigned h) {(void)d;(void)a;(void)b;(void)p;(void)n;(void)t;(void)q;(void)h;return 0;}
int diag_after(diagnostic *d,const ts_decoder *a,const ts_check *b,int f) {(void)d;(void)a;(void)b;(void)f;return 0;}
void diag_issue(void *d,const char *s,unsigned p,uint64_t n,unsigned e,unsigned a) {(void)d;(void)s;(void)p;(void)n;(void)e;(void)a;}
int diag_close(diagnostic *d) {(void)d;return 0;}
int diag_replay_chunks(const char *a,ts_decoder *b,ts_check *c,ts_sink d,void *e,size_t f) {(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0;}
int dt295_capture_run(const dt295_capture_config *c) {(void)c;return 70;}
void ts_check_init(ts_check *c) { memset(c,0,sizeof(*c)); }
void ts_check_packet(ts_check *c,const uint8_t *p) {(void)c;(void)p;}
void ts_decoder_init(ts_decoder *d,const uint8_t *c,ts_sink s,void *u,ts_check *k) {(void)c; memset(d,0,sizeof(*d));d->sink=s;d->user=u;d->check=k;}
int ts_decoder_feed(ts_decoder *d,const uint8_t *p,size_t n) {(void)p;(void)n;if(fault("decoder")){d->error="fake decoder failure";return -1;}d->check->packets++;d->check->pat=1;d->check->pmt=1;return 0;}
int pix_pid_filter(uint8_t *p,size_t *n,const uint8_t *m) {(void)p;(void)n;(void)m;return 0;}
int usb_list_model(int j,int m) {(void)j;(void)m;return 0;}

int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *arg) {
    (void)a; event("worker_create"); if (fault("worker-create")) return -1; worker_fn=fn;worker_arg=arg;worker_created=1;*t=(pthread_t)1;return 0;
}
int __wrap_pthread_join(pthread_t t,void **r) {(void)t;(void)r;event("worker_join");if(worker_created){worker_fn(worker_arg);worker_created=0;}return 0;}
int __wrap_pthread_cond_destroy(pthread_cond_t *c) {(void)c;event("queue_cond_destroy");return 0;}
int __wrap_pthread_mutex_destroy(pthread_mutex_t *m) {(void)m;event("queue_mutex_destroy");return 0;}
