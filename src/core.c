#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pg_prelude.h"

#include "curl_prelude.h"

#include "core.h"
#include "errors.h"
#include "event.h"

static SPIPlanPtr del_response_plan     = NULL;
static SPIPlanPtr del_return_queue_plan = NULL;
static SPIPlanPtr ins_response_plan     = NULL;
static SPIPlanPtr ins_queue_plan        = NULL;
static SPIPlanPtr sel_queue_plan        = NULL;

static size_t body_cb(void *contents, size_t size, size_t nmemb, void *userp) {
  CurlHandle *handle   = (CurlHandle *)userp;
  size_t      realsize = size * nmemb;
  appendBinaryStringInfo(handle->body, (const char *)contents, (int)realsize);
  return realsize;
}

static struct curl_slist *pg_text_array_to_slist(ArrayType *array, struct curl_slist *headers) {
  ArrayIterator iterator;
  Datum         value;
  bool          isnull;
  char         *hdr;

  iterator = array_create_iterator(array, 0, NULL);

  while (array_iterate(iterator, &value, &isnull)) {
    if (isnull) {
      continue;
    }

    hdr = TextDatumGetCString(value);
    EREPORT_CURL_SLIST_APPEND(headers, hdr);
    pfree(hdr);
  }
  array_free_iterator(iterator);

  return headers;
}

void init_curl_handle(CurlHandle *handle, RequestQueueRow row) {
  handle->id        = row.id;
  handle->body      = makeStringInfo();
  handle->ez_handle = curl_easy_init();

  handle->timeout_milliseconds = row.timeout_milliseconds;

  handle->attempts    = row.attempts + 1; // this handle is doing the next attempt
  handle->max_retries = row.max_retries;

  // the request can only be requeued while its columns are still around, and the SPI tuple table
  // they come from is gone by the time the requests finish. Copying is only worth it for requests
  // that can actually be retried.
  handle->headersJson = (NullableDatum){.value = (Datum)0, .isnull = true};
  handle->bodyBin     = (NullableDatum){.value = (Datum)0, .isnull = true};

  if (handle->attempts <= handle->max_retries) {
    if (!row.headersJson.isnull)
      handle->headersJson =
          (NullableDatum){.value = datumCopy(row.headersJson.value, false, -1), .isnull = false};

    if (!row.bodyBin.isnull)
      handle->bodyBin =
          (NullableDatum){.value = datumCopy(row.bodyBin.value, false, -1), .isnull = false};
  }

  if (!row.headersBin.isnull) {
    ArrayType         *pgHeaders       = DatumGetArrayTypeP(row.headersBin.value);
    struct curl_slist *request_headers = NULL;

    request_headers = pg_text_array_to_slist(pgHeaders, request_headers);

    EREPORT_CURL_SLIST_APPEND(request_headers, "User-Agent: pg_net/" EXTVERSION);

    handle->request_headers = request_headers;
  }

  handle->url = TextDatumGetCString(row.url);

  handle->req_body = !row.bodyBin.isnull ? TextDatumGetCString(row.bodyBin.value) : NULL;

  handle->method = TextDatumGetCString(row.method);

  if (strcasecmp(handle->method, "GET") != 0 && strcasecmp(handle->method, "POST") != 0 &&
      strcasecmp(handle->method, "DELETE") != 0) {
    ereport(ERROR, errmsg("Unsupported request method %s", handle->method));
  }

  if (strcasecmp(handle->method, "GET") == 0) {
    if (handle->req_body) {
      EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_POSTFIELDS, handle->req_body);
      EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_CUSTOMREQUEST, "GET");
    }
  }

  if (strcasecmp(handle->method, "POST") == 0) {
    if (handle->req_body) {
      EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_POSTFIELDS, handle->req_body);
    } else {
      EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_POST, 1L);
      EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_POSTFIELDSIZE, 0L);
    }
  }

  if (strcasecmp(handle->method, "DELETE") == 0) {
    EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_CUSTOMREQUEST, "DELETE");
    if (handle->req_body) {
      EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_POSTFIELDS, handle->req_body);
    }
  }

  EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_WRITEFUNCTION, body_cb);
  EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_WRITEDATA, handle);
  EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_HEADER, 0L);
  EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_URL, handle->url);
  EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_HTTPHEADER, handle->request_headers);
  EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_TIMEOUT_MS, (long)handle->timeout_milliseconds);
  EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_PRIVATE, handle);
  EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_FOLLOWLOCATION, (long)true);
  if (LOG_MIN_MESSAGES <= DEBUG2) EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_VERBOSE, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500 /* libcurl 7.85.0 */
  EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_PROTOCOLS_STR, "http,https");
#else
  EREPORT_CURL_SETOPT(handle->ez_handle, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
}

void set_curl_mhandle(WorkerState *wstate) {
  EREPORT_CURL_MULTI_SETOPT(wstate->curl_mhandle, CURLMOPT_SOCKETFUNCTION, multi_socket_cb);
  EREPORT_CURL_MULTI_SETOPT(wstate->curl_mhandle, CURLMOPT_SOCKETDATA, wstate);
  EREPORT_CURL_MULTI_SETOPT(wstate->curl_mhandle, CURLMOPT_TIMERFUNCTION, multi_timer_cb);
  EREPORT_CURL_MULTI_SETOPT(wstate->curl_mhandle, CURLMOPT_TIMERDATA, wstate);
}

uint64 delete_expired_responses(char *ttl, int batch_size) {
  if (del_response_plan == NULL) {
    SPIPlanPtr tmp = SPI_prepare("\
        WITH\
        rows AS (\
          SELECT ctid\
          FROM net._http_response\
          WHERE created < now() - $1\
          ORDER BY created\
          LIMIT $2\
        )\
        DELETE FROM net._http_response r\
        USING rows WHERE r.ctid = rows.ctid",
                                 2, (Oid[]){INTERVALOID, INT4OID});
    if (tmp == NULL)
      ereport(ERROR, errmsg("SPI_prepare failed: %s", SPI_result_code_string(SPI_result)));

    del_response_plan = SPI_saveplan(tmp);
    if (del_response_plan == NULL) ereport(ERROR, errmsg("SPI_saveplan failed"));
  }

  int ret_code = SPI_execute_plan(
      del_response_plan,
      (Datum[]){DirectFunctionCall3(interval_in, CStringGetDatum(ttl), ObjectIdGetDatum(InvalidOid),
                                    Int32GetDatum(-1)),
                Int32GetDatum(batch_size)},
      NULL, false, 0);

  uint64 affected_rows = SPI_processed;

  if (ret_code != SPI_OK_DELETE) {
    ereport(ERROR,
            errmsg("Error expiring response table rows: %s", SPI_result_code_string(ret_code)));
  }

  return affected_rows;
}

uint64 consume_request_queue(const int batch_size) {
  if (del_return_queue_plan == NULL) {
    SPIPlanPtr tmp = SPI_prepare("\
        WITH\
        rows AS (\
          SELECT id\
          FROM net.http_request_queue\
          WHERE next_attempt_at <= clock_timestamp()\
          ORDER BY id\
          LIMIT $1\
        )\
        DELETE FROM net.http_request_queue q\
        USING rows WHERE q.id = rows.id\
        RETURNING q.id, q.method, q.url, timeout_milliseconds, array(select key || ': ' || value from jsonb_each_text(q.headers)), q.body, q.attempts, q.max_retries, q.headers",
                                 1, (Oid[]){INT4OID});

    if (tmp == NULL)
      ereport(ERROR, errmsg("SPI_prepare failed: %s", SPI_result_code_string(SPI_result)));

    del_return_queue_plan = SPI_saveplan(tmp);
    if (del_return_queue_plan == NULL) ereport(ERROR, errmsg("SPI_saveplan failed"));
  }

  int ret_code =
      SPI_execute_plan(del_return_queue_plan, (Datum[]){Int32GetDatum(batch_size)}, NULL, false, 0);

  if (ret_code != SPI_OK_DELETE_RETURNING)
    ereport(ERROR,
            errmsg("Error getting http request queue: %s", SPI_result_code_string(ret_code)));

  return SPI_processed;
}

// This has an implicit dependency on the execution of
// delete_return_request_queue, unfortunately we're not able to make this
// dependency explicit due to the design of SPI (which uses global variables)
RequestQueueRow get_request_queue_row(HeapTuple spi_tupval, TupleDesc spi_tupdesc) {
  bool tupIsNull = false;

  int64 id = DatumGetInt64(SPI_getbinval(spi_tupval, spi_tupdesc, 1, &tupIsNull));
  EREPORT_NULL_ATTR(tupIsNull, id);

  Datum method = SPI_getbinval(spi_tupval, spi_tupdesc, 2, &tupIsNull);
  EREPORT_NULL_ATTR(tupIsNull, method);

  Datum url = SPI_getbinval(spi_tupval, spi_tupdesc, 3, &tupIsNull);
  EREPORT_NULL_ATTR(tupIsNull, url);

  int32 timeout_milliseconds = DatumGetInt32(SPI_getbinval(spi_tupval, spi_tupdesc, 4, &tupIsNull));
  EREPORT_NULL_ATTR(tupIsNull, timeout_milliseconds);

  NullableDatum headersBin = {.value  = SPI_getbinval(spi_tupval, spi_tupdesc, 5, &tupIsNull),
                              .isnull = tupIsNull};

  NullableDatum bodyBin = {.value  = SPI_getbinval(spi_tupval, spi_tupdesc, 6, &tupIsNull),
                           .isnull = tupIsNull};

  int32 attempts = DatumGetInt32(SPI_getbinval(spi_tupval, spi_tupdesc, 7, &tupIsNull));
  EREPORT_NULL_ATTR(tupIsNull, attempts);

  int32 max_retries = DatumGetInt32(SPI_getbinval(spi_tupval, spi_tupdesc, 8, &tupIsNull));
  EREPORT_NULL_ATTR(tupIsNull, max_retries);

  NullableDatum headersJson = {.value  = SPI_getbinval(spi_tupval, spi_tupdesc, 9, &tupIsNull),
                               .isnull = tupIsNull};

  return (RequestQueueRow){id,      method,   url,         timeout_milliseconds, headersBin,
                           bodyBin, attempts, max_retries, headersJson};
}

// true when the queue holds requests scheduled for a later attempt. Requests that are already due
// are not considered, otherwise a direct insertion on the queue would be picked up without a wake.
bool queue_has_scheduled_requests(void) {
  if (sel_queue_plan == NULL) {
    SPIPlanPtr tmp = SPI_prepare("\
        SELECT true FROM net.http_request_queue WHERE next_attempt_at > clock_timestamp() LIMIT 1",
                                 0, NULL);

    if (tmp == NULL)
      ereport(ERROR, errmsg("SPI_prepare failed: %s", SPI_result_code_string(SPI_result)));

    sel_queue_plan = SPI_saveplan(tmp);
    if (sel_queue_plan == NULL) ereport(ERROR, errmsg("SPI_saveplan failed"));

    SPI_freeplan(tmp);
  }

  // not read only, otherwise the requests requeued on this same transaction wouldn't be seen
  int ret_code = SPI_execute_plan(sel_queue_plan, NULL, NULL, false, 1);

  if (ret_code != SPI_OK_SELECT)
    ereport(ERROR,
            errmsg("Error checking the http request queue: %s", SPI_result_code_string(ret_code)));

  return SPI_processed > 0;
}

static Jsonb *jsonb_headers_from_curl_handle(CURL *ez_handle) {
  struct curl_header *header, *prev = NULL;
  PG_JSONB_INIT_STATE(headers);
  (void)PG_JSONB_PUSH(headers, WJB_BEGIN_OBJECT, NULL);

  while ((header = curl_easy_nextheader(ez_handle, CURLH_HEADER, 0, prev))) {
    JsonbValue key   = {.type = jbvString,
                        .val  = {.string = {.val = header->name, .len = strlen(header->name)}}};
    JsonbValue value = {.type = jbvString,
                        .val  = {.string = {.val = header->value, .len = strlen(header->value)}}};
    (void)PG_JSONB_PUSH(headers, WJB_KEY, &key);
    (void)PG_JSONB_PUSH(headers, WJB_VALUE, &value);
    prev = header;
  }

  return PG_JSONB_OBJECT_FINISH(headers);
}

static void insert_response(CurlHandle *handle, CURLcode curl_return_code) {
  enum { nparams = 8 }; // using an enum because const size_t nparams doesn't compile
  Datum vals[nparams];
  char  nulls[nparams];
  MemSet(nulls, 'n', nparams);

  vals[0]  = Int64GetDatum(handle->id);
  nulls[0] = ' ';

  vals[7]  = Int32GetDatum(handle->attempts);
  nulls[7] = ' ';

  if (curl_return_code == CURLE_OK) {
    Jsonb *jsonb_headers        = jsonb_headers_from_curl_handle(handle->ez_handle);
    long   res_http_status_code = 0;

    EREPORT_CURL_GETINFO(handle->ez_handle, CURLINFO_RESPONSE_CODE, &res_http_status_code);

    vals[1]  = Int32GetDatum(res_http_status_code);
    nulls[1] = ' ';

    if (handle->body && handle->body->data[0] != '\0') {
      vals[2]  = CStringGetTextDatum(handle->body->data);
      nulls[2] = ' ';
    }

    vals[3]  = JsonbPGetDatum(jsonb_headers);
    nulls[3] = ' ';

    struct curl_header *hdr;
    if (curl_easy_header(handle->ez_handle, "content-type", 0, CURLH_HEADER, -1, &hdr) ==
        CURLHE_OK) {
      vals[4]  = CStringGetTextDatum(hdr->value);
      nulls[4] = ' ';
    }

    vals[5]  = BoolGetDatum(false);
    nulls[5] = ' ';
  } else {
    bool timed_out = curl_return_code == CURLE_OPERATION_TIMEDOUT;

    vals[5]  = BoolGetDatum(timed_out);
    nulls[5] = ' ';

    if (timed_out) {
      curl_timeout_msg timeout_msg =
          detailed_timeout_strerror(handle->ez_handle, handle->timeout_milliseconds);

      vals[6]  = CStringGetTextDatum(timeout_msg.msg);
      nulls[6] = ' ';
    } else {
      const char *error_msg = curl_easy_strerror(curl_return_code);

      if (error_msg) {
        vals[6]  = CStringGetTextDatum(error_msg);
        nulls[6] = ' ';
      }
    }
  }

  if (ins_response_plan == NULL) {
    SPIPlanPtr tmp = SPI_prepare(
        "\
        insert into net._http_response(id, status_code, content, headers, content_type, timed_out, error_msg, attempts) values ($1, $2, $3, $4, $5, $6, $7, $8)",
        nparams,
        (Oid[nparams]){INT8OID, INT4OID, TEXTOID, JSONBOID, TEXTOID, BOOLOID, TEXTOID, INT4OID});

    if (tmp == NULL)
      ereport(ERROR, errmsg("SPI_prepare failed: %s", SPI_result_code_string(SPI_result)));

    ins_response_plan = SPI_saveplan(tmp);
    if (ins_response_plan == NULL) ereport(ERROR, errmsg("SPI_saveplan failed"));

    SPI_freeplan(tmp);
  }

  int ret_code = SPI_execute_plan(ins_response_plan, vals, nulls, false, 0);

  if (ret_code != SPI_OK_INSERT) {
    ereport(ERROR, errmsg("Error when inserting response: %s", SPI_result_code_string(ret_code)));
  }
}

// Failures that won't change no matter how many times the request is repeated. Everything else
// (connection failures, timeouts, resolving errors, partial transfers) is worth another attempt.
static bool is_retryable_curl_code(CURLcode code) {
  switch (code) {
  case CURLE_UNSUPPORTED_PROTOCOL:
  case CURLE_URL_MALFORMAT:
  case CURLE_NOT_BUILT_IN:
  case CURLE_UNKNOWN_OPTION:
  case CURLE_BAD_FUNCTION_ARGUMENT:
  case CURLE_TOO_MANY_REDIRECTS:
  case CURLE_PEER_FAILED_VERIFICATION:
  case CURLE_SSL_CACERT_BADFILE:
  case CURLE_SSL_CIPHER:
  case CURLE_LOGIN_DENIED            : return false;
  default                            : return true;
  }
}

// Only the status codes that signal a transient condition on the server side
static bool is_retryable_status(long status_code) {
  return status_code == 408    // Request Timeout
         || status_code == 425 // Too Early
         || status_code == 429 // Too Many Requests
         || (status_code >= 500 && status_code <= 599);
}

// The delay a `Retry-After` response header asks for, in milliseconds. Negative when the header is
// absent or cannot be understood, both its delay-seconds and its HTTP-date form are accepted.
static int64 retry_after_milliseconds(CURL *ez_handle) {
  struct curl_header *hdr;

  if (curl_easy_header(ez_handle, "retry-after", 0, CURLH_HEADER, -1, &hdr) != CURLHE_OK) return -1;

  char *end       = NULL;
  errno           = 0;
  long delay_secs = strtol(hdr->value, &end, 10);

  if (errno == 0 && end != hdr->value) {
    while (*end == ' ' || *end == '\t')
      end++;

    if (*end == '\0') return delay_secs > 0 ? (int64)delay_secs * 1000 : 0;
  }

  time_t retry_at = curl_getdate(hdr->value, NULL);

  if (retry_at == -1) return -1;

  double delay = difftime(retry_at, time(NULL));

  return delay > 0 ? (int64)(delay * 1000) : 0;
}

// Exponential backoff on the number of attempts already made
static int64 backoff_milliseconds(int32 attempts, RetryPolicy policy) {
  int shift = attempts - 1;

  if (shift > 20) // the cap below makes any bigger shift pointless, and keeps the shift in range
    shift = 20;

  return (int64)policy.base_delay_milliseconds << shift;
}

static bool should_retry(CurlHandle *handle, CURLcode curl_return_code, RetryPolicy policy,
                         int64 *delay_milliseconds) {
  if (handle->attempts > handle->max_retries) // no retries left
    return false;

  int64 delay = -1;

  if (curl_return_code == CURLE_OK) {
    long status_code = 0;

    EREPORT_CURL_GETINFO(handle->ez_handle, CURLINFO_RESPONSE_CODE, &status_code);

    if (!is_retryable_status(status_code)) return false;

    delay = retry_after_milliseconds(handle->ez_handle);
  } else if (!is_retryable_curl_code(curl_return_code)) {
    return false;
  }

  if (delay < 0) delay = backoff_milliseconds(handle->attempts, policy);

  *delay_milliseconds = Min(delay, (int64)policy.max_delay_milliseconds);

  return true;
}

// Puts the request back on the queue, keeping its id so the caller still gets a single response
// row once the request is done retrying
static void requeue_request(CurlHandle *handle, int64 delay_milliseconds) {
  enum { nparams = 9 };
  Datum vals[nparams];
  char  nulls[nparams];
  MemSet(nulls, ' ', nparams);

  vals[0] = Int64GetDatum(handle->id);
  vals[1] = CStringGetTextDatum(handle->method);
  vals[2] = CStringGetTextDatum(handle->url);

  vals[3]  = handle->headersJson.value;
  nulls[3] = handle->headersJson.isnull ? 'n' : ' ';

  vals[4]  = handle->bodyBin.value;
  nulls[4] = handle->bodyBin.isnull ? 'n' : ' ';

  vals[5] = Int32GetDatum(handle->timeout_milliseconds);
  vals[6] = Int32GetDatum(handle->attempts);
  vals[7] = Int32GetDatum(handle->max_retries);
  vals[8] = Float8GetDatum((double)delay_milliseconds);

  if (ins_queue_plan == NULL) {
    SPIPlanPtr tmp = SPI_prepare("\
        insert into net.http_request_queue(id, method, url, headers, body, timeout_milliseconds, attempts, max_retries, next_attempt_at)\
        values ($1, $2, $3, $4, $5, $6, $7, $8, clock_timestamp() + $9 * interval '1 millisecond')",
                                 nparams,
                                 (Oid[nparams]){INT8OID, TEXTOID, TEXTOID, JSONBOID, BYTEAOID,
                                                INT4OID, INT4OID, INT4OID, FLOAT8OID});

    if (tmp == NULL)
      ereport(ERROR, errmsg("SPI_prepare failed: %s", SPI_result_code_string(SPI_result)));

    ins_queue_plan = SPI_saveplan(tmp);
    if (ins_queue_plan == NULL) ereport(ERROR, errmsg("SPI_saveplan failed"));

    SPI_freeplan(tmp);
  }

  int ret_code = SPI_execute_plan(ins_queue_plan, vals, nulls, false, 0);

  if (ret_code != SPI_OK_INSERT) {
    ereport(ERROR, errmsg("Error when requeuing request: %s", SPI_result_code_string(ret_code)));
  }
}

void complete_request(CurlHandle *handle, CURLcode curl_return_code, RetryPolicy policy) {
  int64 delay_milliseconds = 0;

  if (!should_retry(handle, curl_return_code, policy, &delay_milliseconds)) {
    insert_response(handle, curl_return_code);
    return;
  }

  ereport(LOG, errmsg("pg_net retrying request " INT64_FORMAT " in " INT64_FORMAT
                      " ms, attempt %d of %d failed",
                      handle->id, delay_milliseconds, handle->attempts, handle->max_retries + 1));

  requeue_request(handle, delay_milliseconds);
}

void pfree_handle(CurlHandle *handle) {
  pfree(handle->url);
  pfree(handle->method);
  if (handle->req_body) pfree(handle->req_body);

  if (!handle->headersJson.isnull) pfree(DatumGetPointer(handle->headersJson.value));
  if (!handle->bodyBin.isnull) pfree(DatumGetPointer(handle->bodyBin.value));

  if (handle->body) destroyStringInfo(handle->body);

  if (handle->request_headers) // curl_slist_free_all already handles the NULL
                               // case, but be explicit about it
    curl_slist_free_all(handle->request_headers);
}
