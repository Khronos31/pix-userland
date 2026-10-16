#ifndef PIX_USB_BACKEND_H
#define PIX_USB_BACKEND_H
#include "pixy.h"
#include "usb_selector.h"
typedef struct usb_backend usb_backend;
typedef int (*usb_sink)(void *, const uint8_t *, size_t);
enum { USB_MODEL_DT181 = 181, USB_MODEL_DT295 = 295 };
int usb_model(const usb_backend *);
void *usb_native_context(usb_backend *);
void *usb_native_handle(usb_backend *);
int usb_open_model(usb_backend **, const usb_selector *, int fd, int detach, int expected_model);
/* Enumerate only one requested model. usb_list() remains the legacy DT181-only API. */
int usb_list_model(int json, int expected_model);
void usb_diagnostics(usb_backend *, int enabled);
int usb_list(int json);
/* Returns 0, or a shared CLI exit class: 3 absent, 4 busy/kernel-bound,
 * 7 USB/access/disconnect, 70 internal. Failure sets *result to NULL. */
int usb_open(usb_backend **, const usb_selector *, int fd, int detach);
/* First failure, preserved through cleanup: 0 healthy, 7 USB, 8 sink,
 * 70 internal/diagnostics. NULL is an internal failure (70). */
int usb_failure_exit(const usb_backend *);
pixy_io usb_io(usb_backend *);
/* A clean stop permits restart. Failed streams remain failed until usb_close.
 * Do not start again before stop has drained callbacks and closed the logger. */
int usb_stream_start(usb_backend *, usb_sink, void *);
int usb_poll(usb_backend *, unsigned ms);
int usb_stream_stop(usb_backend *);
void usb_close(usb_backend *);
#endif
