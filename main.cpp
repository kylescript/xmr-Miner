// main.cpp: 定义控制台应用程序的入口点。
//
#include <stdio.h>
#include <uv.h>
#include "xmr_proxy.h"

char recv_buff[2048] = { 0 };
int recv_buff_len = 0;
int recv_buff_pos = 0;

static void on_close(uv_handle_t* handle)
{
	printf("client sockent closed.");
}

void on_write(uv_write_t *req, int status) {
	if (status < 0) {
		fprintf(stderr, "write error %s\n", uv_err_name(status));
	}
	free(req);
}

static void alloc_cb(uv_handle_t* handle, size_t size, uv_buf_t* buf) {
	buf->base = recv_buff;
	buf->len = size;
}

static void mxr_proxy_write(uv_stream_t* stream, char * buffer, size_t len) {
	printf("on_send: %s", buffer);

	auto buf = uv_buf_init(buffer, len);

	uv_write_t *request = (uv_write_t*)malloc(sizeof(uv_write_t));
	request->write_buffer = uv_buf_init(buffer, len);
	uv_write(request, stream, &buf, 1, on_write);
}

static void on_read(uv_stream_t* tcp, ssize_t nread, const uv_buf_t* buf)
{
	if (nread < 0) {
		if (nread != UV_EOF) {
			fprintf(stderr, "on_read error %s\n", uv_strerror(nread));
		}
		return uv_close((uv_handle_t*)tcp, on_close);
	}

	if (nread == 0) {
		fprintf(stderr, "on_read nread == 0.\n");
		return;
	}

	char tmp[2048] = { 0 };
	memcpy_s(tmp, 2048, buf->base, nread);
	printf("on_recv: %s\n", tmp);

	recv_buff_len = nread + recv_buff_pos;
	char * start = buf->base;
	char * end = 0;
	while ((end = (char *)memchr(start, '\n', recv_buff_len)) != 0) {
		end++;

		size_t len = end - start;
		xmr_proxy_parse(mxr_proxy_write, tcp, start, len);
		start = end;
		recv_buff_len -= len;
	}

	if (start == buf->base) {
		return;
	}

	if (recv_buff_len == 0) {
		recv_buff_pos = 0;
		return;
	}

	recv_buff_pos = recv_buff_len;
	memcpy(buf->base, start, recv_buff_len);

	uv_read_start(tcp, alloc_cb, on_read);
}

void on_connect(uv_connect_t* req, int status) {
	if (status < 0) {
		fprintf(stderr, "connect server error %s\n", uv_strerror(status));
		return;
	}

	// login the pool
	printf("Connected!\nPrepare sending login request...\n");
	uv_stream_t* stream = req->handle;
	char * login_msg = (char *)"{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"login\",\"params\":{\"login\":\"42FFZH8JtBGSvqWurocPgR8pzaCp383J2dUASYnXSBhpYbutkP5wQ6TeisR3eAxGV5ZPEPGSUy8AcGtmeX3tRiLgNXi3444;10000+Test\",\"pass\":\"x\",\"agent\":\"DuoduoMiner v1.0\"}}\n";
	mxr_proxy_write(stream, login_msg, strlen(login_msg));
	uv_read_start(stream, alloc_cb, on_read);
}

int main() {
	auto loop = uv_default_loop();
	uv_tcp_t* socket = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));
	uv_tcp_init(loop, socket);

	uv_connect_t* connect = (uv_connect_t*)malloc(sizeof(uv_connect_t));
	struct sockaddr dest;
	//ping mine.ppxxmr.com, find out the ip address.
	uv_ip4_addr("222.187.239.46", 3333, (sockaddr_in*)&dest);
	uv_tcp_connect(connect, socket, &dest, on_connect);

	auto r = uv_run(loop, UV_RUN_DEFAULT);
	uv_loop_close(loop);
	uv_close((uv_handle_t*)socket, NULL);
	free(connect);
	free(socket);
	return r;
}
