#ifndef PIX_USB_BACKEND_H
#define PIX_USB_BACKEND_H
#include "pixy.h"
typedef struct usb_backend usb_backend;
typedef int (*usb_sink)(void *, const uint8_t *, size_t);
int usb_list(void);
int usb_open(usb_backend **, unsigned index, int fd, int detach);
pixy_io usb_io(usb_backend *);
int usb_stream_start(usb_backend *, usb_sink, void *);
int usb_poll(usb_backend *, unsigned ms);
int usb_stream_stop(usb_backend *);
void usb_close(usb_backend *);
#endif
