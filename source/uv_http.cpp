#include "stdafx.h"
#include "uv_http.h"
#include <assert.h>
#include <exception>
#include <http_context.h>
#include <logger.h>


namespace pigeon {

	namespace http {



		typedef struct {

			uv_tcp_t handle;
			http_parser parser;
			uv_write_t write_req;
			http_context* context;

		} client_t;

		typedef struct {

			uv_work_t request;
			client_t* client;
			bool error;
			char* result;
			size_t length;

		} msg_baton_t;

		namespace server {

			void on_connect(uv_stream_t* server_handle, int status){
				try {

					assert((uv_tcp_t*)server_handle == &uv_tcp);

					client_t* client = (client_t*)malloc(sizeof(client_t));

					client->context = new http_context;
					client->context->Settings = _Settings;
					client->context->Cache = _Cache;


					uv_tcp_init(uv_loop, &client->handle);
					http_parser_init(&client->parser, HTTP_REQUEST);

					client->parser.data = client;
					client->handle.data = client;

					int r = uv_accept(server_handle, (uv_stream_t*)&client->handle);
					if (r != 0){
						logger::get(_Settings)->write(LogType::Error, Severity::Critical, uv_err_name(r));
					}
					uv_read_start((uv_stream_t*)&client->handle,
						[&](uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
						*buf = uv_buf_init((char *)malloc(suggested_size), suggested_size);
					},
						on_read);


				}
				catch (std::exception& ex){
					throw std::runtime_error(ex.what());
				}
			}

			void on_read(uv_stream_t* tcp, ssize_t nread, const uv_buf_t* buf){
				try {

					ssize_t parsed;
					client_t *client = (client_t *)tcp->data;
					if (nread >= 0) {
						parsed = (ssize_t)http_parser_execute(
							&client->parser, &parser_settings, buf->base, nread);
						if (parsed < nread) {
							logger::get(_Settings)->write(LogType::Error, Severity::Critical, "parse failed");
							uv_close((uv_handle_t *)&client->handle, on_close);
						}
					}
					else {
						if (nread != UV_EOF) {
							logger::get(_Settings)->write(LogType::Error, Severity::Critical, "read failed");
						}
						uv_close((uv_handle_t *)&client->handle, on_close);
					}
					free(buf->base);
				}
				catch (std::exception& ex){

					throw std::runtime_error(ex.what());

				}
			}

			void on_render(uv_work_t *req){
				try {

					msg_baton_t *closure = static_cast<msg_baton_t *>(req->data);
					client_t* client = (client_t*)closure->client;

					process(client->context);

					closure->result = (char*)client->context->response->message.c_str();
					closure->length = client->context->response->message.size();

				}
				catch (std::exception& ex) {
					throw std::runtime_error(ex.what());
				}
			}

			void on_render_complete(uv_work_t *req){
				try {

					msg_baton_t *closure = static_cast<msg_baton_t *>(req->data);
					client_t* client = (client_t*)closure->client;


					uv_buf_t resbuf;
					resbuf.base = closure->result;
					resbuf.len = (ULONG)closure->length;

					client->write_req.data = closure;

					int r = uv_write(&client->write_req,
						(uv_stream_t*)&client->handle,
						&resbuf,
						1, 
						on_send_complete);

					if (r != 0){
						logger::get(_Settings)->write(LogType::Error, Severity::Critical, uv_err_name(r));
					}

				}
				catch (std::exception& ex) {
					throw std::runtime_error(ex.what());
				}
			}

			void on_send_complete(uv_write_t *req, int status){

				try {

					if (status != 0){
						logger::get(_Settings)->write(LogType::Error, Severity::Critical, uv_err_name(status));
					}
					if (!uv_is_closing((uv_handle_t*)req->handle))
					{
						msg_baton_t *closure = static_cast<msg_baton_t *>(req->data);
						delete closure;
						uv_close((uv_handle_t*)req->handle, on_close);
					}

				}
				catch (std::exception& ex){
					throw std::runtime_error(ex.what());
				}

			}

			void on_close(uv_handle_t *handle){

				try {

					client_t* client = (client_t*)handle->data;
					delete client->context;
					free(client);

				}
				catch (std::exception ex) {

				}
			}

		}

		namespace parser {

			auto on_url(http_parser* parser, const char* at, size_t len) -> int {

				client_t* client = (client_t*)parser->data;
				if (at && client->context->request) {
					string s(at, len);
					client->context->request->url = s;
				}
				return 0;

			}

			auto on_header_field(http_parser* parser, const char* at, size_t len) -> int {

				client_t* client = (client_t*)parser->data;
				if (at && client->context->request) {
					string s(at, len);
					client->context->request->set_header_field(s);
				}
				return 0;

			}

			auto on_header_value(http_parser* parser, const char* at, size_t len) -> int {

				client_t* client = (client_t*)parser->data;
				if (at && client->context->request) {
					string s(at, len);
					client->context->request->set_header_value(s);
				}
				return 0;

			}

			auto on_headers_complete(http_parser* parser) -> int {

				client_t* client = (client_t*)parser->data;
				client->context->request->method = parser->method;
				client->context->request->http_major_version = parser->http_major;
				client->context->request->http_minor_version = parser->http_minor;
				return 0;

			}

			auto on_body(http_parser* parser, const char* at, size_t len) -> int {

				client_t* client = (client_t*)parser->data;
				if (at && client->context->request) {
					string s(at, len);
					client->context->request->content = s;
				}
				return 0;

			}

			auto on_message_complete(http_parser* parser) -> int {

				client_t* client = (client_t*)parser->data;
				msg_baton_t *closure = new msg_baton_t();
				closure->request.data = closure;
				closure->client = client;
				closure->error = false;

				client->context->request->is_api = is_api(client->context->request->url);
				parse_query_string(*client->context->request);

				int status = uv_queue_work(uv_loop,
					&closure->request,
					[](uv_work_t *req) { server::on_render(req); },
					[](uv_work_t *req, int status) { server::on_render_complete(req); });
				return status;

			}

		}
	}
}