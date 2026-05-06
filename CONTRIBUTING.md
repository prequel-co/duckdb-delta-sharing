# Contributing to DuckDB Delta Share

First off, thank you for considering contributing to DuckDB Delta Share! It's people like you that make it such a great tool.

## Getting Started

1. Fork the repo and create your branch from `main`.
2. Make sure you have `cmake` and a C++ compiler installed.

## Building the Extension

To build the extension locally:

```bash
make release
```

## Testing

The project contains End-to-End integration tests under `test/integration/`.
These tests connect to a live Delta Sharing server (e.g. Databricks). To run them, you'll need to set up a `.env` file at the root of the project with your test server credentials:

```env
DELTA_SHARING_ENDPOINT=https://your-delta-sharing-server.com/api/2.0/delta-sharing
DELTA_SHARING_BEARER_TOKEN=your_private_token
```

Then, you can run the integration tests:

```bash
./test/integration/run_e2e_tests.sh
```

## Code Formatting

This project uses `clang-format` and `clang-tidy` to enforce C++ style conventions.
Please ensure your code passes the linting checks before submitting a PR.
