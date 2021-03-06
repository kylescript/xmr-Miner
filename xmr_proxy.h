#ifndef __XMR_PROXY_H__
#define __XMR_PROXY_H__

#include <uv.h>

typedef void(*xmr_proxy_write_cb)(uv_stream_t*, 
								  char*, 
								  size_t);

void xmr_proxy_parse(xmr_proxy_write_cb cb, uv_stream_t* stream, char *buf, size_t len);
#endif //__XMR_PROXY_H__
