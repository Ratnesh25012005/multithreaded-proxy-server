# Multithreaded Proxy Server and Client

A C-based operating systems project that forwards HTTP requests through a multithreaded proxy server and lets a client save the returned page as an HTML file.

> **Status:** Planning and documentation. The implementation, build commands, and test results will be added as development progresses.

## Overview

A proxy server acts as an intermediary between a client and a destination web server. Instead of fetching a page directly, the client sends its URL to the proxy. The proxy retrieves the response and sends it back to the client.

This project demonstrates socket programming, concurrent request handling, and resource management. It is based on **Project 6: Multi-threaded Proxy Server and Client** from the proposed OS project list.

## Project Requirements

- Develop a multithreaded proxy server and a client.
- Accept a URL from the client, fetch the corresponding page, and return the response.
- Allow the client to save the returned page as an HTML file with an appropriate filename.

## Planned Features

- **URL input:** Accept a destination URL through a command-line client.
- **Request forwarding:** Connect to the destination server and forward an HTTP GET request.
- **Concurrent clients:** Handle multiple client connections using POSIX threads.
- **HTML output:** Save the response body to a user-specified or generated `.html` filename.
- **Error handling:** Report invalid input, connection failures, and file-write failures.
- **Resource cleanup:** Close sockets and files and release allocated memory after requests finish.

The initial implementation is planned for plain HTTP. HTTPS support requires additional design and is not included in the initial scope. Response caching is an optional extension.

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

## Planned Technology Stack

| Component | Technology |
| --- | --- |
| Programming language | C |
| Network communication | TCP sockets and HTTP |
| Concurrency | POSIX threads (`pthread`) |
| Synchronization, where needed | Mutexes or semaphores |
| Development environment | Linux, or a Linux environment through WSL on Windows |
| Compiler | GCC |
| Version control | Git and GitHub |

## Development Setup

The intended development environment needs:

- A Linux environment with networking access.
- GCC and the development headers needed for sockets and POSIX threads.
- Git for version control.
- A text editor or IDE.

Build and execution commands will be documented after the source files and command-line interface are implemented and tested. No runnable server or client is included yet.

## Implementation Plan

1. Define how the client sends a URL and how the proxy returns the response.
2. Implement a single-client HTTP request and response flow.
3. Add client-side saving of the HTML response body.
4. Add worker threads to support concurrent client connections.
5. Add input validation, error reporting, and resource cleanup.
6. Test the required behavior and document reproducible build and run commands.
7. Consider optional caching after the core requirements pass.

## Validation Plan

These checks are planned; they have not been run yet.

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
- Record request logs and response times.
- Compare sequential and concurrent request handling.
- Replace unbounded thread creation with a worker pool or a connection limit.
- Add connection and transfer timeouts.

## Academic Context

Developed for the CSE 302 Operating Systems course. Team details and implementation results will be added when available.
