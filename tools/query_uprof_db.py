#!/usr/bin/env python3
"""Read an AMD uProf DuckDB database through uProf's bundled DuckDB C API."""

from __future__ import annotations

import argparse
import ctypes
from pathlib import Path


class DuckDBResult(ctypes.Structure):
    _fields_ = [
        ("deprecated_column_count", ctypes.c_uint64),
        ("deprecated_row_count", ctypes.c_uint64),
        ("deprecated_rows_changed", ctypes.c_uint64),
        ("deprecated_columns", ctypes.c_void_p),
        ("deprecated_error_message", ctypes.c_void_p),
        ("internal_data", ctypes.c_void_p),
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("database", type=Path)
    parser.add_argument("sql")
    parser.add_argument(
        "--dll",
        type=Path,
        default=Path(r"C:\Program Files\AMD\AMDuProf\bin\duckdb.dll"),
    )
    args = parser.parse_args()

    library = ctypes.WinDLL(str(args.dll.resolve()))
    database = ctypes.c_void_p()
    connection = ctypes.c_void_p()
    config = ctypes.c_void_p()
    error = ctypes.c_void_p()

    library.duckdb_create_config.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    library.duckdb_create_config.restype = ctypes.c_int
    library.duckdb_set_config.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
    ]
    library.duckdb_set_config.restype = ctypes.c_int
    library.duckdb_open_ext.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    library.duckdb_open_ext.restype = ctypes.c_int
    library.duckdb_connect.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    library.duckdb_connect.restype = ctypes.c_int
    library.duckdb_query.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.POINTER(DuckDBResult),
    ]
    library.duckdb_query.restype = ctypes.c_int
    library.duckdb_column_count.argtypes = [ctypes.POINTER(DuckDBResult)]
    library.duckdb_column_count.restype = ctypes.c_uint64
    library.duckdb_row_count.argtypes = [ctypes.POINTER(DuckDBResult)]
    library.duckdb_row_count.restype = ctypes.c_uint64
    library.duckdb_column_name.argtypes = [
        ctypes.POINTER(DuckDBResult),
        ctypes.c_uint64,
    ]
    library.duckdb_column_name.restype = ctypes.c_char_p
    library.duckdb_value_varchar.argtypes = [
        ctypes.POINTER(DuckDBResult),
        ctypes.c_uint64,
        ctypes.c_uint64,
    ]
    library.duckdb_value_varchar.restype = ctypes.c_void_p
    library.duckdb_result_error.argtypes = [ctypes.POINTER(DuckDBResult)]
    library.duckdb_result_error.restype = ctypes.c_char_p
    library.duckdb_free.argtypes = [ctypes.c_void_p]
    library.duckdb_destroy_result.argtypes = [ctypes.POINTER(DuckDBResult)]
    library.duckdb_disconnect.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    library.duckdb_close.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    library.duckdb_destroy_config.argtypes = [ctypes.POINTER(ctypes.c_void_p)]

    if library.duckdb_create_config(ctypes.byref(config)) != 0:
        raise RuntimeError("duckdb_create_config failed")
    try:
        if library.duckdb_set_config(
            config, b"access_mode", b"read_only"
        ) != 0:
            raise RuntimeError("cannot set DuckDB read-only mode")
        state = library.duckdb_open_ext(
            str(args.database.resolve()).encode(),
            ctypes.byref(database),
            config,
            ctypes.byref(error),
        )
        if state != 0:
            message = ctypes.string_at(error).decode(errors="replace") if error else ""
            raise RuntimeError(f"duckdb_open_ext failed: {message}")
    finally:
        library.duckdb_destroy_config(ctypes.byref(config))
        if error:
            library.duckdb_free(error)

    try:
        if library.duckdb_connect(database, ctypes.byref(connection)) != 0:
            raise RuntimeError("duckdb_connect failed")
        result = DuckDBResult()
        try:
            if library.duckdb_query(
                connection, args.sql.encode(), ctypes.byref(result)
            ) != 0:
                message = library.duckdb_result_error(ctypes.byref(result))
                raise RuntimeError(
                    message.decode(errors="replace") if message else "query failed"
                )
            columns = library.duckdb_column_count(ctypes.byref(result))
            rows = library.duckdb_row_count(ctypes.byref(result))
            print("\t".join(
                library.duckdb_column_name(ctypes.byref(result), column)
                .decode(errors="replace")
                for column in range(columns)
            ))
            for row in range(rows):
                values = []
                for column in range(columns):
                    value = library.duckdb_value_varchar(
                        ctypes.byref(result), column, row
                    )
                    if value:
                        values.append(
                            ctypes.string_at(value).decode(errors="replace")
                        )
                        library.duckdb_free(value)
                    else:
                        values.append("NULL")
                print("\t".join(values))
        finally:
            library.duckdb_destroy_result(ctypes.byref(result))
    finally:
        if connection:
            library.duckdb_disconnect(ctypes.byref(connection))
        if database:
            library.duckdb_close(ctypes.byref(database))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
