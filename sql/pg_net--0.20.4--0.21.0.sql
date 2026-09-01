alter table net.http_request_queue
  add column if not exists max_retries int not null default 0 check (max_retries >= 0),
  add column if not exists attempts int not null default 0,
  add column if not exists next_attempt_at timestamptz not null default now();

alter table net._http_response
  add column if not exists attempts int not null default 1;

-- the request functions gain `max_retries` and `idempotency_key` arguments, drop the previous
-- signatures so the new ones don't end up as ambiguous overloads
drop function if exists net.http_get(text, jsonb, jsonb, integer);
drop function if exists net.http_post(text, jsonb, jsonb, jsonb, integer);
drop function if exists net.http_delete(text, jsonb, jsonb, integer, jsonb);

create or replace function net._with_idempotency_key(headers jsonb, idempotency_key text)
    returns jsonb
    language sql
    immutable
as $$
    select coalesce(
        (
            select jsonb_object_agg(key, value)
            from jsonb_each(coalesce(headers, '{}'::jsonb))
            where lower(key) <> 'idempotency-key'
        ),
        '{}'::jsonb
    ) || jsonb_build_object('Idempotency-Key', idempotency_key);
$$;

create or replace function net.http_get(
    -- url for the request
    url text,
    -- key/value pairs to be url encoded and appended to the `url`
    params jsonb default '{}'::jsonb,
    -- key/values to be included in request headers
    headers jsonb default '{}'::jsonb,
    -- the maximum number of milliseconds the request may take before being cancelled
    timeout_milliseconds int default 5000,
    -- how many times a failed request will be retried, zero disables retries
    max_retries int default 0,
    -- value for the `Idempotency-Key` header, so a retry is not taken as a new request
    idempotency_key text default null
)
    -- request_id reference
    returns bigint
    language plpgsql
as $$
declare
    request_id bigint;
    params_array text[];
begin
    select coalesce(array_agg(net._urlencode_string(key) || '=' || net._urlencode_string(value)), '{}')
    into params_array
    from jsonb_each_text(params);

    if idempotency_key is not null then
        headers := net._with_idempotency_key(headers, idempotency_key);
    end if;

    -- Add to the request queue
    insert into net.http_request_queue(method, url, headers, timeout_milliseconds, max_retries)
    values (
        'GET',
        net._encode_url_with_params_array(url, params_array),
        headers,
        timeout_milliseconds,
        max_retries
    )
    returning id
    into request_id;

    perform net.wake();

    return request_id;
end
$$;

create or replace function net.http_post(
    -- url for the request
    url text,
    -- body of the POST request
    body jsonb default '{}'::jsonb,
    -- key/value pairs to be url encoded and appended to the `url`
    params jsonb default '{}'::jsonb,
    -- key/values to be included in request headers
    headers jsonb default '{"Content-Type": "application/json"}'::jsonb,
    -- the maximum number of milliseconds the request may take before being cancelled
    timeout_milliseconds int DEFAULT 5000,
    -- how many times a failed request will be retried, zero disables retries
    max_retries int default 0,
    -- value for the `Idempotency-Key` header, so a retry is not taken as a new request
    idempotency_key text default null
)
    -- request_id reference
    returns bigint
    language plpgsql
as $$
declare
    request_id bigint;
    params_array text[];
    content_type text;
begin

    -- Exctract the content_type from headers
    select
        header_value into content_type
    from
        jsonb_each_text(coalesce(headers, '{}'::jsonb)) r(header_name, header_value)
    where
        lower(header_name) = 'content-type'
    limit
        1;

    -- If the user provided new headers and omitted the content type
    -- add it back in automatically
    if content_type is null then
        select headers || '{"Content-Type": "application/json"}'::jsonb into headers;
    end if;

    -- Confirm that the content-type is "application/json" or a Kafka REST
    -- Proxy JSON media type (application/vnd.kafka.<embedded>+json, e.g.
    -- application/vnd.kafka.json.v2+json). The body parameter is jsonb so
    -- the wire payload is always JSON-shaped; this guard exists only to
    -- catch obvious mismatches between the declared header and the body.
    if content_type <> 'application/json'
       and content_type not like 'application/vnd.kafka.%+json' then
        raise exception 'Content-Type header must be "application/json" or "application/vnd.kafka.<variant>+json"';
    end if;

    select
        coalesce(array_agg(net._urlencode_string(key) || '=' || net._urlencode_string(value)), '{}')
    into
        params_array
    from
        jsonb_each_text(params);

    if idempotency_key is not null then
        headers := net._with_idempotency_key(headers, idempotency_key);
    end if;

    -- Add to the request queue
    insert into net.http_request_queue(method, url, headers, body, timeout_milliseconds, max_retries)
    values (
        'POST',
        net._encode_url_with_params_array(url, params_array),
        headers,
        convert_to(body::text, 'UTF8'),
        timeout_milliseconds,
        max_retries
    )
    returning id
    into request_id;

    perform net.wake();

    return request_id;
end
$$;

create or replace function net.http_delete(
    -- url for the request
    url text,
    -- key/value pairs to be url encoded and appended to the `url`
    params jsonb default '{}'::jsonb,
    -- key/values to be included in request headers
    headers jsonb default '{}'::jsonb,
    -- the maximum number of milliseconds the request may take before being cancelled
    timeout_milliseconds int default 5000,
    -- optional body of the request
    body jsonb default NULL,
    -- how many times a failed request will be retried, zero disables retries
    max_retries int default 0,
    -- value for the `Idempotency-Key` header, so a retry is not taken as a new request
    idempotency_key text default null
)
    -- request_id reference
    returns bigint
    language plpgsql
as $$
declare
    request_id bigint;
    params_array text[];
begin
    select coalesce(array_agg(net._urlencode_string(key) || '=' || net._urlencode_string(value)), '{}')
    into params_array
    from jsonb_each_text(params);

    if idempotency_key is not null then
        headers := net._with_idempotency_key(headers, idempotency_key);
    end if;

    -- Add to the request queue
    insert into net.http_request_queue(method, url, headers, body, timeout_milliseconds, max_retries)
    values (
        'DELETE',
        net._encode_url_with_params_array(url, params_array),
        headers,
        convert_to(body::text, 'UTF8'),
        timeout_milliseconds,
        max_retries
    )
    returning id
    into request_id;

    perform net.wake();

    return request_id;
end
$$;
