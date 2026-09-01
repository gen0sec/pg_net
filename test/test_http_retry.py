import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

import pytest
from sqlalchemy import text
from common import collect_response_sync, http_request

# ports of the servers this module starts, they only need to not clash with the nginx one
flaky_port = 8091
late_port = 8092

# how many requests each `key` has received so far
seen = {}
# the `Idempotency-Key` header each `key` arrived with, one entry per attempt
idempotency_keys = {}
seen_lock = threading.Lock()


class FlakyHandler(BaseHTTPRequestHandler):
    """Fails a configurable number of times before answering with a 200"""

    def do_GET(self):
        query = parse_qs(urlsplit(self.path).query)
        param = lambda name, default: query.get(name, [default])[0]

        length = int(self.headers.get("content-length", 0) or 0)
        body = self.rfile.read(length).decode() if length else ""

        key = param("key", "")
        fails = int(param("fail", "0"))
        status = int(param("status", "503"))
        delay = float(param("delay", "0"))

        with seen_lock:
            seen[key] = seen.get(key, 0) + 1
            attempt = seen[key]
            idempotency_keys.setdefault(key, []).append(self.headers.get("Idempotency-Key"))

        # a negative `fail` never succeeds
        failing = fails < 0 or attempt <= fails

        if failing and delay > 0:
            time.sleep(delay)

        if not failing:
            # echoing back what was sent shows a retried request keeps its headers and body
            return self.reply(200, "ok " + self.headers.get("x-echo", "") + " " + body)

        if status == 429:
            return self.reply(429, "slow down", headers=[("Retry-After", param("retry_after", "1"))])

        self.reply(status, "failed")

    do_POST = do_GET
    do_DELETE = do_GET

    def reply(self, status, body, headers=()):
        payload = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(payload)))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        pass


def serve(port):
    server = ThreadingHTTPServer(("127.0.0.1", port), FlakyHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


@pytest.fixture(scope="module", autouse=True)
def flaky_server():
    server = serve(flaky_port)
    yield server
    server.shutdown()


def request_count(key):
    """How many attempts the server received for `key`"""
    with seen_lock:
        return seen.get(key, 0)


def idempotency_keys_of(key):
    """The Idempotency-Key header of every attempt the server received for `key`"""
    with seen_lock:
        return list(idempotency_keys.get(key, []))


def retry_row(sess, request_id):
    """
    The retry related columns of a response, plus how many responses the request got.

    A request gets a single response no matter how many times it was retried, so the
    count is there to catch a retry leaving an extra row behind.
    """
    return sess.execute(
        text("""
        select timed_out, attempts, count(*) over () as responses
        from net._http_response where id = :request_id
        """),
        {"request_id": request_id},
    ).mappings().fetchone()


def queue_length(sess, request_id):
    return sess.execute(
        text("select count(*) from net.http_request_queue where id = :request_id"),
        {"request_id": request_id},
    ).scalar_one()


def test_no_retries_by_default(sess):
    """a failed request is not retried unless max_retries is given"""
    request_id = http_request(sess, text(
        f"""
        select net.http_get('http://127.0.0.1:{flaky_port}/always?key=default&fail=-1');
    """
    ))

    response = collect_response_sync(sess, request_id)

    assert response["status"] == "SUCCESS"
    assert response["status_code"] == 503
    assert retry_row(sess, request_id)["attempts"] == 1
    assert request_count("default") == 1


def test_retries_until_max_retries_is_reached(sess):
    """a request that keeps failing is attempted max_retries + 1 times and gets a single response"""
    request_id = http_request(sess, text(
        f"""
        select net.http_get('http://127.0.0.1:{flaky_port}/always?key=exhaust&fail=-1', max_retries := 2);
    """
    ))

    response = collect_response_sync(sess, request_id)
    row = retry_row(sess, request_id)

    assert response["status_code"] == 503
    assert row["attempts"] == 3
    assert row["responses"] == 1
    assert request_count("exhaust") == 3
    # the request is not left behind on the queue
    assert queue_length(sess, request_id) == 0


def test_stops_retrying_once_the_request_succeeds(sess):
    """retries stop at the first successful attempt"""
    request_id = http_request(sess, text(
        f"""
        select net.http_get('http://127.0.0.1:{flaky_port}/flaky?key=recovers&fail=2', max_retries := 5);
    """
    ))

    response = collect_response_sync(sess, request_id)

    assert response["status_code"] == 200
    assert retry_row(sess, request_id)["attempts"] == 3
    assert request_count("recovers") == 3


def test_retries_a_post_with_its_headers_and_body(sess):
    """a retried request is sent again with the same headers and body"""
    request_id = http_request(sess, text(
        f"""
        select net.http_post(
            url := 'http://127.0.0.1:{flaky_port}/flaky?key=post&fail=1',
            body := '{{"hello": "world"}}'::jsonb,
            headers := '{{"Content-Type": "application/json", "x-echo": "echoed"}}'::jsonb,
            max_retries := 1
        );
    """
    ))

    response = collect_response_sync(sess, request_id)

    assert response["status_code"] == 200
    assert retry_row(sess, request_id)["attempts"] == 2
    assert "echoed" in response["body"]
    assert "hello" in response["body"]


def test_sends_the_same_idempotency_key_on_every_attempt(sess):
    """the idempotency_key lets the server tell a retry apart from a new request"""
    request_id = http_request(sess, text(
        f"""
        select net.http_post(
            url := 'http://127.0.0.1:{flaky_port}/flaky?key=idempotent&fail=2',
            body := '{{"hello": "world"}}'::jsonb,
            max_retries := 2,
            idempotency_key := 'a-stable-key'
        );
    """
    ))

    response = collect_response_sync(sess, request_id)

    assert response["status_code"] == 200
    assert retry_row(sess, request_id)["attempts"] == 3
    assert idempotency_keys_of("idempotent") == ["a-stable-key"] * 3


def test_idempotency_key_replaces_a_header_of_any_case(sess):
    """the argument wins over an Idempotency-Key already present on the headers"""
    request_id = http_request(sess, text(
        f"""
        select net.http_get(
            url := 'http://127.0.0.1:{flaky_port}/flaky?key=idempotent-header',
            headers := '{{"idempotency-key": "from-the-headers"}}'::jsonb,
            idempotency_key := 'from-the-argument'
        );
    """
    ))

    assert collect_response_sync(sess, request_id)["status"] == "SUCCESS"
    assert idempotency_keys_of("idempotent-header") == ["from-the-argument"]


def test_no_idempotency_key_is_sent_by_default(sess):
    """the header is only sent when the argument is given"""
    request_id = http_request(sess, text(
        f"""
        select net.http_get('http://127.0.0.1:{flaky_port}/flaky?key=no-idempotency');
    """
    ))

    assert collect_response_sync(sess, request_id)["status"] == "SUCCESS"
    assert idempotency_keys_of("no-idempotency") == [None]


def test_does_not_retry_a_non_retryable_status(sess):
    """statuses that won't change on a retry, like 404, are returned right away"""
    request_id = http_request(sess, text(
        f"""
        select net.http_get('http://127.0.0.1:{flaky_port}/always?key=notfound&fail=-1&status=404', max_retries := 3);
    """
    ))

    response = collect_response_sync(sess, request_id)

    assert response["status_code"] == 404
    assert retry_row(sess, request_id)["attempts"] == 1
    assert request_count("notfound") == 1


def test_retries_a_timed_out_request(sess):
    """a request that times out is attempted again"""
    request_id = http_request(sess, text(
        f"""
        select net.http_get(
            url := 'http://127.0.0.1:{flaky_port}/flaky?key=timeout&fail=1&delay=2',
            timeout_milliseconds := 500,
            max_retries := 1
        );
    """
    ))

    response = collect_response_sync(sess, request_id)
    row = retry_row(sess, request_id)

    assert response["status_code"] == 200
    assert row["timed_out"] == False
    assert row["attempts"] == 2


def test_retries_a_connection_refused_until_the_server_is_up(sess):
    """transport errors are retried, so a request survives a server that is momentarily down"""
    request_id = http_request(sess, text(
        f"""
        select net.http_get('http://127.0.0.1:{late_port}/flaky?key=late', max_retries := 5);
    """
    ))

    server = None
    try:
        time.sleep(1.5)
        server = serve(late_port)

        response = collect_response_sync(sess, request_id)

        assert response["status_code"] == 200
        assert retry_row(sess, request_id)["attempts"] > 1
    finally:
        if server:
            server.shutdown()


def test_honors_the_retry_after_header(sess, autocommit_sess):
    """a Retry-After header takes precedence over the exponential backoff"""
    # a backoff this long makes it obvious that the Retry-After delay is the one being used
    autocommit_sess.execute(text("alter system set pg_net.retry_base_delay_milliseconds = 300000"))
    autocommit_sess.execute(text("select pg_reload_conf()"))

    try:
        request_id = http_request(sess, text(
            f"""
            select net.http_get(
                'http://127.0.0.1:{flaky_port}/always?key=retry-after&fail=1&status=429&retry_after=1',
                max_retries := 1
            );
        """
        ))

        started = time.monotonic()

        response = collect_response_sync(sess, request_id)

        elapsed = time.monotonic() - started

        assert response["status_code"] == 200
        assert retry_row(sess, request_id)["attempts"] == 2
        assert elapsed < 60
    finally:
        autocommit_sess.execute(text("alter system reset pg_net.retry_base_delay_milliseconds"))
        autocommit_sess.execute(text("select pg_reload_conf()"))


def test_max_retries_cannot_be_negative(sess):
    """the queue rejects a negative max_retries"""
    with pytest.raises(Exception) as execinfo:
        sess.execute(text(
            f"""
            select net.http_get('http://127.0.0.1:{flaky_port}/', max_retries := -1);
        """
        ))

    assert "http_request_queue_max_retries_check" in str(execinfo.value)
