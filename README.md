# Multithreaded Proxy Server and Client

A C-based operating systems project that forwards HTTP requests through a multithreaded proxy server and lets a client save the returned page as an HTML file.

> **Status:** Milestones 1-3 implemented: plain HTTP forwarding, HTML saving, and bounded multithreading in both server and batch client. HTTPS and redirect following are not implemented yet.

## Overview

A proxy server acts as an intermediary between a client and a destination web server. Instead of fetching a page directly, the client sends its URL to the proxy. The proxy retrieves the response and sends it back to the client.

This project demonstrates socket programming, concurrent request handling, and resource management. It is based on **Project 6: Multi-threaded Proxy Server and Client** from the proposed OS project list.

## Project Requirements

- Develop a multithreaded proxy server and a client.
- Accept a URL from the client, fetch the corresponding page, and return the response.
- Allow the client to save the returned page as an HTML file with an appropriate filename.

## Implemented Features

- **URL input:** Accept a plain HTTP URL through the command-line client, including a path, query string, and explicit port. URL fragments are not sent to the website.
- **Request forwarding:** The proxy resolves the destination and forwards an HTTP GET request. The client contacts the proxy rather than fetching the website directly.
- **Concurrent clients:** Serve up to 32 active connections with detached POSIX worker threads. Excess connections receive HTTP 503.
- **Threaded batch downloads:** Download a URL list with 1-16 client workers (default 4). Each job owns its socket and output file; all filenames are assigned before workers start.
- **Request logs:** Server logs carry request identifiers, worker start/finish events, and relayed byte counts. Batch results identify each URL and summarize saved and failed jobs.
- **HTML output:** Save the response body to a chosen filename or generate one under `downloads/`. Automatic names receive numeric suffixes on collision. Explicit filenames are never overwritten.
- **Response handling:** Read split headers, Content-Length, chunked transfer coding, and connection-close-delimited bodies. Only successful HTML responses with identity content encoding are saved.
- **Failure handling:** Report invalid input, connection failures, destination HTTP errors, unsupported responses, and file-write failures. Failed downloads do not leave completed output files.
- **Resource cleanup:** Close sockets and files and release per-request memory. Download into a temporary file and publish it only on success.

The current implementation supports plain HTTP. HTTPS and redirect following are later milestones needed for broader webpage support. Response caching is an optional extension.

## How It Works

```text
Client                         Proxy Server                   Destination Server
  |                                  |                                |
  |---------- Send URL ------------->|                                |
  |                                  |----- Forward HTTP request ---->|
  |                                  |<-------- HTTP response --------|
  |<--------- Return response -------|                                |
  |                                  |                                |
  +-- Save response body as HTML      +-- Clean up request resources   |
```

The proxy will accept incoming connections and assign each accepted connection to a worker thread. Each worker will handle its own request and destination connection. Any shared mutable state introduced later, such as a cache, will need synchronization.

Saving an HTML response does not download all linked images, stylesheets, or scripts. A complete offline website downloader is outside the initial scope.

## Technology Stack

| Component | Technology |
| --- | --- |
| Programming language | C |
| Network communication | TCP sockets and HTTP |
| Concurrency | POSIX threads (`pthread`) |
| Connection limit | POSIX semaphore |
| Development environment | Linux, or a Linux environment through WSL on Windows |
| Compiler | GCC |
| Version control | Git and GitHub |

## Development Setup

The intended development environment needs:

- A Linux environment with networking access.
- GCC and the development headers needed for sockets and POSIX threads.
- Git for version control.
- A text editor or IDE.

The project was built and tested in Ubuntu through WSL. Run the following commands in a Linux terminal, from the project directory. Native Windows MinGW is not the target for this POSIX implementation.

```bash
make
```

Start the proxy in one terminal:

```bash
./proxy_server 8080
```

In another terminal, fetch an HTTP page through the proxy:

```bash
./proxy_client 127.0.0.1 8080 http://example.com/ --output page.html
```

Omit `--output` to generate a filename:

```bash
./proxy_client 127.0.0.1 8080 http://example.com/
```

Public sites may redirect to HTTPS or reject automated requests. For a repeatable offline demonstration, create a small HTML page and start a local origin server in a third terminal:

```bash
mkdir -p /tmp/os-proxy-demo
printf '<html><body>Hello from our proxy project!</body></html>\n' > /tmp/os-proxy-demo/index.html
python3 -m http.server 8000 --bind 127.0.0.1 --directory /tmp/os-proxy-demo
```

Then run the client, keeping the proxy running:

```bash
./proxy_client 127.0.0.1 8080 http://127.0.0.1:8000/index.html --output demo.html
```

The output shows the HTTP status, saved body size, and filename. An existing explicit output file causes an error; choose another name or omit `--output` for automatic collision handling.

The listener binds to `127.0.0.1`, so the client and proxy should run in the same Linux environment for this milestone. Stop the proxy with Ctrl+C.

## Multithreaded Batch Client

Keep the proxy and local demo origin running, then run:

```bash
./proxy_client 127.0.0.1 8080 --batch examples/urls.txt --jobs 3
```

The example list requests the local HTML page three times with different query strings. For other pages, create a text file containing one HTTP URL per line. Blank lines and lines starting with `#` are ignored; the whole list is validated before downloads begin.

Each run creates a unique `downloads/batch_XXXXXX/` directory. Files are named with the job number and sanitized hostname/path, such as `0001_127.0.0.1_index.html`. Duplicate URLs and simultaneous batch runs cannot select the same output path.

The client creates no more than `--jobs` worker threads, up to 16. A mutex protects the shared job index; network requests run outside that lock. It joins its workers before reporting the final result. The server independently limits active workers to 32 using a semaphore and releases each slot when its worker exits.

Failed jobs do not stop unrelated downloads. The final summary reports saved/failed counts; the process exits with a nonzero status if any job failed. A batch accepts at most 1000 URLs. `--output` is available only in single-URL mode.

## Source Files

| File | Purpose |
| --- | --- |
| `proxy_server.c` | Accept client connections, create bounded workers, fetch and relay HTTP responses. |
| `proxy_client.c` | Fetch one URL or a batch through the proxy, decode response framing, and save HTML. |
| `net.c` / `net.h` | URL validation, destination resolution, timed connections, and complete socket writes. |
| `tests/integration.py` | Local HTTP fixtures and end-to-end checks. |
| `tests/concurrency.py` | Client worker limits, server saturation/recovery, timeouts, disconnects, and connection churn. |
| `examples/urls.txt` | Small batch input for the local demo origin. |
| `proxy_server_with_cache.c` / `proxy_server_without_cache.c` | Original tutorial reference implementations, preserved separately. |
| `proxy_parse.c` / `proxy_parse.h` | Original tutorial parser; not linked into the new milestone executables. |

`make tutorial` builds the preserved cached tutorial as `tutorial_proxy`. It is a reference target and does not include our dedicated HTML-saving client or fixes.

## Implementation Plan

The HTTP server, HTML-saving client, and concurrency milestone are implemented. Next: add HTTPS and redirects. Consider caching after the required workflow is complete.

## Validation Plan

Run the deterministic local checks with Python 3:

```bash
make test
```

For memory and undefined-behavior checks on the new executables:

```bash
make sanitize
make test
```

Return to the normal build afterward:

```bash
make clean
make
```

Normal and AddressSanitizer/UndefinedBehaviorSanitizer builds each passed 29 end-to-end scenarios and 10 concurrency scenarios, with no reported sanitizer errors. These include 16 concurrent downloads, a bounded threaded batch, 410 sequential requests, all 32 server slots in use, overload rejection/recovery, a client reset, and an idle-request timeout. Tests use a local origin and do not require a public website.

| Scenario | Expected behavior |
| --- | --- |
| Valid HTTP URL returning HTML | Client receives the page and saves its body to an HTML file. |
| Multiple clients at the same time | Requests complete independently without mixed response data. |
| Invalid URL or unsupported scheme | Client receives a clear error. |
| Unreachable destination | Request fails cleanly without stopping the proxy. |
| Destination returns an HTTP error | Client can identify the returned HTTP status. |
| Response larger than one buffer | Data is transferred without truncation. |
| Client disconnects during transfer | Worker cleans up its resources and the server remains available. |
| Output file cannot be written | Client reports the file error. |

## Current Limitations

- Plain HTTP GET only. HTTPS, automatic redirects, authentication, cookies, and JavaScript execution are not supported yet.
- The client saves HTML/XHTML only and requests uncompressed data. If the origin still returns compressed content, the client rejects it rather than save incorrect HTML.
- Downloads are limited to 50 MiB. Client response headers are limited to 64 KiB; incoming proxy requests are limited to 16 KiB.
- Socket transfer operations time out after 15 seconds without progress; connection attempts use a 10-second timeout per destination address. System DNS resolution follows the resolver's own timeout behavior.
- A close-delimited response has no declared body length, so an early clean EOF cannot always be distinguished from a complete response. Declared-length and chunked truncation are detected.
- The server serves at most 32 active requests. Multiple batch clients can exceed that limit and receive HTTP 503; automatic retries are not implemented.
- Completed files are finalized using POSIX hard links. Use a filesystem that supports them. No cache is enabled in the project executables.

## Concepts to Understand for the Viva

- **Proxy server:** Why a client sends requests through an intermediary.
- **Sockets:** How the client, proxy, and destination establish connections.
- **Threads and processes:** Why this design uses threads to handle concurrent clients.
- **Concurrency and parallelism:** How multiple requests can make progress at once.
- **HTTP:** How a request, status line, headers, and response body fit together.
- **TCP streams:** Why one send or receive operation may not transfer an entire message.
- **Synchronization:** When shared data needs protection and when request-local data does not.
- **Resource management:** How sockets, thread resources, files, and memory are released.

## Possible Extensions

- Add a bounded, thread-safe response cache with an LRU eviction policy.
- Add request timing and performance measurements to the existing request logs.
- Compare sequential and concurrent request handling.
- Compare the current bounded per-connection server workers with a persistent worker pool.
- Add HTTPS and redirect handling.

## Academic Context

Developed for the CSE 302 Operating Systems course. Team details and implementation results will be added when available.
