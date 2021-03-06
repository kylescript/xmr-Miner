#include "xmr_proxy.h"
#include <assert.h>
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include <iostream>
#include <time.h>
#include <vector>
#include <mutex>
#include <thread>
#include <chrono>
#include "malloc.h"
#include <string.h>
#include "crypto/CryptoNight.h"

using namespace rapidjson;

class Job;
time_t get_cur_timestamp();
std::vector<Job> jobs;
std::mutex jobs_mutex;

xmr_proxy_write_cb write_callback = nullptr;
uv_stream_t* write_callback_param = nullptr;
char pool_id[256] = { 0 };

static inline char hf_bin2hex(unsigned char c)
{
	if (c <= 0x9) {
		return '0' + c;
	}

	return 'a' - 0xA + c;
}

static inline unsigned char hf_hex2bin(char c, bool &err)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	else if (c >= 'a' && c <= 'f') {
		return c - 'a' + 0xA;
	}
	else if (c >= 'A' && c <= 'F') {
		return c - 'A' + 0xA;
	}

	err = true;
	return 0;
}


bool fromHex(const char* in, unsigned int len, unsigned char* out)
{
	bool error = false;
	for (unsigned int i = 0; i < len; i += 2) {
		out[i / 2] = (hf_hex2bin(in[i], error) << 4) | hf_hex2bin(in[i + 1], error);

		if (error) {
			return false;
		}
	}
	return true;
}


void toHex(const unsigned char* in, unsigned int len, char* out)
{
	for (unsigned int i = 0; i < len; i++) {
		out[i * 2] = hf_bin2hex((in[i] & 0xF0) >> 4);
		out[i * 2 + 1] = hf_bin2hex(in[i] & 0x0F);
	}
}

class Job {
public:
	Job(const char*blob, const char *target, const char *job_id) {
		strcpy_s(m_blob, blob);
		strcpy_s(m_target, target);
		strcpy_s(m_job_id, job_id);
		m_is_zero_start = true;
		timestamp = 0;
	};
public:
	char m_blob[512];
	char m_target[64];
	char m_job_id[64];
	bool m_is_zero_start;
	time_t timestamp;
};

uint64_t get_target(char *target) {
	const size_t len = strlen(target);
	if (len <= 8) {
		uint32_t tmp = 0;
		char str[8];
		memcpy(str, target, len);

		if (!fromHex(str, 8, reinterpret_cast<unsigned char*>(&tmp)) || tmp == 0) {
			return 0;
		}
		return 0xFFFFFFFFFFFFFFFFULL / (0xFFFFFFFFULL / static_cast<uint64_t>(tmp));
	}
	else if (len <= 16) {
		target = 0;
		char str[16];
		memcpy(str, target, len);

		uint64_t result = 0;
		if (!fromHex(str, 16, reinterpret_cast<unsigned char*>(&result)) || result == 0) {
			return 0;
		}
		return result;
	}
	return 0;
}

void work_thread() {
	struct cryptonight_ctx *ctx = (struct cryptonight_ctx*) _mm_malloc(sizeof(struct cryptonight_ctx), 16);
	ctx->memory = (uint8_t *)_mm_malloc(MEMORY * 2, 16);

	while (1) {
		Job my_job("","","");

		{
			std::lock_guard<std::mutex> guard(jobs_mutex);
			if (jobs.size() == 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}

			my_job = jobs[jobs.size() - 1];
			jobs.pop_back();

			//不需要保存那么多Job，反正也做不完
			if (jobs.size() >= 20) {
				jobs.clear();
			}
		}

		// 解析JOB 
		// 获取blob
		unsigned char blob[256] = { 0 };
		uint32_t* blob_nonce =  reinterpret_cast<uint32_t*>(blob + 39);
		int bolb_len = strlen(my_job.m_blob) / 2;
		fromHex(my_job.m_blob, bolb_len*2, blob);
		if (!my_job.m_is_zero_start) {
			*blob_nonce = 0x7fffffff;
		}

		// 获取target
		uint64_t target = get_target(my_job.m_target);
		if (target == 0) {
				printf("bad job. invalid target.");
				continue;
		}

		// 定义result
		unsigned char result[32] = { 0 };
		uint64_t* result_value = reinterpret_cast<uint64_t*>(result + 24);
		
		time_t start_timestamp = get_cur_timestamp();
		//每1000次检查一下超时, 240秒超时
		int x = 0;
		while (1) {
			if (++x % 1000 == 0 ) {
				printf("speed:%.2f H/s.\n", x / (get_cur_timestamp() - start_timestamp) * 1.0);
				if (get_cur_timestamp() - my_job.timestamp > 240) {
					break;
				}
			}

			*blob_nonce = *blob_nonce + 1;
			CryptoNight::hash(blob, bolb_len, result, ctx);
			if (*result_value < target) {
				printf("finished job. will send to pool!\n");
				std::string send_str = "{\"method\":\"submit\",\"params\":{\"id\":\"";
				send_str += pool_id;
				send_str += "\",\"job_id\":\"";
				send_str += my_job.m_job_id;
				send_str += "\",\"nonce\":\"";
				char nonce_str[64] = {};
				toHex((unsigned char*)blob_nonce, 4, nonce_str);
				send_str += nonce_str;
				send_str += "\",\"result\":\"";
				char result_str[128] = {};
				toHex(result, 32, result_str);
				send_str += result_str;
				send_str += "\"},\"id\":1}\n";
				write_callback(write_callback_param, (char *)send_str.c_str(), send_str.length());
				//break;
			}
		}
		//printf("Times:%d\n", x);
	}
}

time_t get_cur_timestamp() {
	time_t rawtime;
	time(&rawtime);
	return rawtime;
}

void xmr_proxy_parse(xmr_proxy_write_cb cb, uv_stream_t* stream, char *buf, size_t len) {
	write_callback = cb;
	write_callback_param = stream;

	assert(buf[len - 1] == '\n');
	buf[len - 1] = '\0';

	Document d;
	if (d.ParseInsitu(buf).HasParseError()) {
		printf("xmr_proxy_parse invalid json format:%s.\n", buf);
		return;
	}
	if (!d.IsObject()) {
		printf("xmr_proxy_parse invalid json format, is not object:%s.\n", buf);
		return;
	}

	if (d.HasMember("id")) {
		if (d.HasMember("error")) {
			if (!d["error"].IsNull()) {
				printf("xmr_proxy_parse:%s\n.", d["error"]["message"].GetString());
				return;
			}
		}
		const Value& result = d["result"];

		// for login with new job
		if (result.HasMember("job")) {
			if (d["error"].IsNull()) {
				printf("xmr_proxy_parse login successfully!\n");
				strcpy_s(pool_id, result["id"].GetString());
				const Value& job = result["job"];

				Job j(job["blob"].GetString(), job["target"].GetString(), job["job_id"].GetString());
				j.timestamp = get_cur_timestamp();
				std::lock_guard<std::mutex> guard(jobs_mutex);
				jobs.push_back(j);
				j.m_is_zero_start = false;
				jobs.push_back(j);

				std::thread t1(work_thread);
				std::thread t2(work_thread);
				//std::thread t3(work_thread);
				//std::thread t4(work_thread);

				t1.detach();
				t2.detach();
				//t3.detach();
				//t4.detach();
			}
			else {
				printf("xmr_proxy_parse login failed:%s\n.", result.GetString());
			}
		}
		// for result of submit
		else {
			if (d.HasMember("error")) {
				if (d["error"].IsNull()) {
					printf("xmr_proxy_parse pool accept the share!\n");
				}
				else {
					printf("xmr_proxy_parse pool reject the share! message:%d\n.", d["error"]["message"].GetString());
				}
			}
		}
	}
	// for new job
	else {
		if (d.HasMember("method")) {
			assert(strcmp(d["method"].GetString(), "job") == 0);
			const Value& job = d["params"];

			Job j(job["blob"].GetString(), job["target"].GetString(), job["job_id"].GetString());
			j.timestamp = get_cur_timestamp();
			std::lock_guard<std::mutex> guard(jobs_mutex);
			jobs.push_back(j);
			j.m_is_zero_start = false;
			jobs.push_back(j);
		}
	}
}
