#ifndef PIX_USB_BACKEND_H
#define PIX_USB_BACKEND_H
#include "pixy.h"
#include "usb_selector.h"
typedef struct usb_backend usb_backend;
typedef int (*usb_sink)(void *, const uint8_t *, size_t);
void usb_diagnostics(usb_backend *, int enabled);
int usb_list(int json);
int usb_open(usb_backend **, const usb_selector *, int fd, int detach);
pixy_io usb_io(usb_backend *);
/* A clean stop permits restart. Failed streams remain failed until usb_close.
 * Do not start again before stop has drained callbacks and closed the logger. */
int usb_stream_start(usb_backend *, usb_sink, void *);
int usb_poll(usb_backend *, unsigned ms);
int usb_stream_stop(usb_backend *);
void usb_close(usb_backend *);
#endif
