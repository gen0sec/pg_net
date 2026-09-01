#ifndef CORE_H
#define CORE_H

typedef enum {
  WS_NOT_YET = 1,
  WS_RUNNING,
  WS_EXITED,
} WorkerStatus;

// the state of the background worker
typedef struct {
  pg_atomic_uint32  got_restart;
  pg_atomic_uint32  should_wake;
  pg_atomic_uint32  status;
  Latch            *shared_latch;
  ConditionVariable cv; // required to publish the state of the worker to other backends
  int               epfd;
  CURLM            *curl_mhandle;
} WorkerState;

// A row coming from the http_request_queue
typedef struct {
  int64         id;
  Datum         method;
  Datum         url;
  int32         timeout_milliseconds;
  NullableDatum headersBin;
  NullableDatum bodyBin;
  int32         attempts;
  int32         max_retries;
  NullableDatum headersJson; // the raw jsonb headers, only needed to requeue a retried request
} RequestQueueRow;

// How long to wait before retrying a failed request
typedef struct {
  int base_delay_milliseconds;
  int max_delay_milliseconds;
} RetryPolicy;

// The curl easy handle plus additional data, this acts for both the request and
// response cycle
typedef struct {
  int64              id;
  StringInfo         body;
  struct curl_slist *request_headers;
  int32              timeout_milliseconds;
  char              *url;
  char              *req_body;
  char              *method;
  CURL              *ez_handle;
  // attempts made for this request, including the one this handle is doing
  int32 attempts;
  int32 max_retries;
  // copies of the queue row columns, only kept when the request can be retried
  NullableDatum headersJson;
  NullableDatum bodyBin;
} CurlHandle;

uint64 delete_expired_responses(char *ttl, int batch_size);

uint64 consume_request_queue(const int batch_size);

RequestQueueRow get_request_queue_row(HeapTuple spi_tupval, TupleDesc spi_tupdesc);

void set_curl_mhandle(WorkerState *wstate);

// inserts the response, or requeues the request when the failure is retryable
void complete_request(CurlHandle *handle, CURLcode curl_return_code, RetryPolicy policy);

bool queue_has_scheduled_requests(void);

void init_curl_handle(CurlHandle *handle, RequestQueueRow row);

void pfree_handle(CurlHandle *handle);

#endif
