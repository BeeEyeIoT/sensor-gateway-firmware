#include "influx_sender.h"
#include "appconfig.h"
#include "messages.h"

#include <stddef.h>

#include <zephyr/sys/base64.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/socket.h> 
#include <zephyr/net/net_ip.h> 
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_DECLARE(app, LOG_LEVEL_DBG);

#define RECV_BUF_SIZE 1024

static int build_influx_line(char *buf, size_t len, const struct measure *measure)
{
  char saddr[17];
  snprintf(saddr, sizeof(saddr), "%02x%02x%02x%02x%02x%02x%02x%02x", 
    measure->sensor_id[0],
    measure->sensor_id[1],
    measure->sensor_id[2],
    measure->sensor_id[3],
    measure->sensor_id[4],
    measure->sensor_id[5],
    measure->sensor_id[6],
    measure->sensor_id[7]
  );

  switch (measure->type) {
  case MEASURE_TYPE_BATTERY:
    return snprintf(buf, len, "battery_mv,sensor=%s value=%u", saddr, measure->data.bat.mV);      
  case MEASURE_TYPE_TEMPERATURE:
    return snprintf(buf, len, "temperature,sensor=%s value=%f", saddr, (double)measure->data.th.tempC); 
  case MEASURE_TYPE_TEMP_AND_HUMIDITY:
    return snprintf(buf, len, "temperature,sensor=%s value=%f\nhumidity,sensor=%s value=%f", 
                        saddr, (double)measure->data.th.tempC, saddr, (double)measure->data.th.hum);
  case MEASURE_TYPE_WEIGHT:
    return snprintf(buf, len, "weight,sensor=%s value=%f", saddr, (double)measure->data.w.weight); 
  default:
    return -EINVAL;
  }

  return 0; // supress warning (doesn't reach here)
}

static int parse_http_url(const char *url, char *host, size_t host_len, uint16_t *port, const char **path)
{
	const char *p;
	const char *host_start;
	const char *host_end;
	const char *port_start = NULL;

	if (!url || !host || !port || !path) {
		return -EINVAL;
	}

	/* Only support http:// */
	if (strncmp(url, "http://", 7) != 0) {
		return -EINVAL;
	}

	p = url + 7; /* skip scheme */

	host_start = p;

	/* find end of host */
	while (*p && *p != ':' && *p != '/') {
		p++;
	}

	host_end = p;

	/* check for port */
	if (*p == ':') {
		p++;
		port_start = p;

		while (*p && *p != '/') {
			p++;
		}
	}

	/* path */
	*path = (*p == '/') ? p : "/";

	/* copy host */
	size_t len = host_end - host_start;
	if (len >= host_len) {
		return -ENOMEM;
	}

	memcpy(host, host_start, len);
	host[len] = '\0';

	/* parse port */
	if (port_start) {
		*port = (uint16_t)atoi(port_start);
	} else {
		*port = 80;
	}

	return 0;
}

static int resolve_ipv4(const char *host, uint16_t port, struct sockaddr_in *out_addr)
{
	struct zsock_addrinfo hints;
	struct zsock_addrinfo *res = NULL;
	int ret;

	if (!host || !out_addr) {
		return -EINVAL;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	LOG_DBG("Resolving %s", host);
	ret = zsock_getaddrinfo(host, NULL, &hints, &res);
	if (ret != 0) {
		LOG_ERR("DNS resolve failed for %s (%d)", host, ret);
		return -EIO;
	}

	struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;

	memset(out_addr, 0, sizeof(*out_addr));
	out_addr->sin_family = AF_INET;
	out_addr->sin_port = htons(port);
	out_addr->sin_addr = addr->sin_addr;

	char ip_str[NET_IPV4_ADDR_LEN];
	zsock_inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
	LOG_DBG("resolved %s -> %s:%u", host, ip_str, port);

	zsock_freeaddrinfo(res);

	return 0;
}

char recv_buf[RECV_BUF_SIZE + 1];
static uint16_t http_status_code;
static int response_cb(struct http_response *rsp, enum http_final_call final_data, void *user_data)
{
	if (final_data == HTTP_DATA_MORE) {
		LOG_DBG("Partial data received (%zd bytes)", rsp->data_len);
	} else if (final_data == HTTP_DATA_FINAL) {
		LOG_DBG("All the data received (%zd bytes)", rsp->data_len);
	}
	LOG_DBG("Response to %s", (const char *)user_data);
	LOG_DBG("Response status %s", rsp->http_status);

	http_status_code = rsp->http_status_code;

	return 0;
}

static int build_basic_auth(char *out, size_t out_len, const char *user, const char *pass)
{
	char tmp[128];
	size_t olen;

	int ret = snprintf(tmp, sizeof(tmp), "%s:%s", user, pass);
	if (ret < 0 || ret >= sizeof(tmp)) {
		return -ENOMEM;
	}

	ret = base64_encode(out, out_len, &olen, (const uint8_t *)tmp, strlen(tmp));
	if (ret) {
		return ret;
	}

	out[olen] = '\0';
	return 0;
}


static int influx_post(const char *body)
{
	struct http_request req = {0};
	struct sockaddr_in addr = {0};
	int sock;
	int ret;
	char host[64];
	uint16_t port;
	const char *path;

	// 1. Parse URL
	ret = parse_http_url(app_config.influx_url, host, sizeof(host), &port, &path);
	if (ret < 0) {
		LOG_ERR("URL parse failed (%d)", ret);
		return ret;
	}

	// 2. Resolve host
	ret = resolve_ipv4(host, port, &addr);
	if (ret < 0) {
		LOG_ERR("DNS resolve failed (%d)", ret);
		return ret;
	}

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		LOG_ERR("socket failed (%d)", errno);
		return -errno;
	}

	// 3. Connect to the HTTP endpoint
	ret = zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		LOG_ERR("connect failed (%d)", errno);
		zsock_close(sock);
		return -errno;
	}

	// 4. Create HTTP POST reqiest
	memset(&req, 0, sizeof(req));
	req.method = HTTP_POST;
	req.url = path;
	req.host = host;
	req.protocol = "HTTP/1.1";
	req.payload = body;
	req.payload_len = strlen(body);
	req.recv_buf = recv_buf;
	req.recv_buf_len = sizeof(recv_buf);
	req.response = response_cb;


	char auth_b64[128];
	char auth_hdr[160];

	ret = build_basic_auth(auth_b64, sizeof(auth_b64), app_config.influx_user, app_config.influx_pass);
	if (ret < 0) {
		LOG_ERR("auth build failed (%d)", ret);
		zsock_close(sock);
		return ret;
	}

	snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Basic %s\r\n", auth_b64);

	const char *headers[4];
	headers[0] = auth_hdr;
	headers[1] = "Content-Type: text/plain\r\n";
	headers[2] = "Connection: close\r\n";
	headers[3] = NULL;

	req.header_fields = headers;

	LOG_DBG("POST %s %d %s", host, port, path);
	LOG_DBG("BODY %s", body);
	ret = http_client_req(sock, &req, 5000, NULL);
	zsock_close(sock);

	if (ret < 0) {
		LOG_ERR("HTTP POST failed (%d)", ret);
		return ret;
	}

	LOG_DBG("HTTP POST OK");

	if (http_status_code >= 200 && http_status_code < 300) {
		return 0;
	}

	if (http_status_code == 401 || http_status_code == 403) {
		return -EACCES;
	}

	if (http_status_code == 404) {
		return -ENOENT;
	}

	if (http_status_code >= 400 && http_status_code < 500) {
		return -EINVAL;
	}

	if (http_status_code >= 500) {
		return -EIO;
	}

	return -EIO;
}

int influx_post_measure(const struct measure *m)
{
	char body[256];
	int ret;

	ret = build_influx_line(body, sizeof(body), m);
	if (ret < 0) {
		LOG_ERR("line build failed: %d", ret);
		return ret;
	}

	ret = influx_post(body);
	if (ret) {
    LOG_ERR("posting metrics failed: %d", ret);
	}
	
	return ret;
}



