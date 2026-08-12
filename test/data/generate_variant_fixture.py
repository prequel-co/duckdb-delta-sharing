# /// script
# requires-python = ">=3.10"
# dependencies = ["pyarrow>=17"]
# ///
"""Generate a Parquet file mimicking a Databricks Delta variant column.

The column is physically struct<metadata: BINARY, value: BINARY> with NO
Parquet VARIANT logical-type annotation. Blob contents follow the Variant
Binary Encoding spec (same encoding the Parquet VARIANT logical type uses).
"""
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

OUT_PATH = Path(__file__).parent / "variant_unannotated.parquet"

# Variant metadata with an empty dictionary:
#   header 0x01 -> version=1, sorted=0, offset_size=1
#   dictionary_size = 0, one offset entry = 0
EMPTY_META = bytes([0x01, 0x00, 0x00])


def meta_with_keys(keys):
    # version=1, sorted=1, offset_size=1
    header = 0x01 | 0x10
    out = bytearray([header, len(keys)])
    offsets = [0]
    blob = bytearray()
    for k in keys:
        blob.extend(k.encode())
        offsets.append(len(blob))
    out.extend(offsets)
    out.extend(blob)
    return bytes(out)


def v_int8(n):
    # primitive (basic_type=0), type_id 3 = int8
    return bytes([(3 << 2) | 0, n & 0xFF])


def v_short_string(s):
    b = s.encode()
    assert len(b) < 64
    return bytes([(len(b) << 2) | 1]) + b


def v_bool_true():
    return bytes([(1 << 2) | 0])


def v_object(meta_keys, fields):
    # object basic_type=2; small object: field_offset_size=1, field_id_size=1,
    # is_large=0 -> value_header = 0
    field_ids = []
    offsets = [0]
    values = bytearray()
    for key, val in fields:
        field_ids.append(meta_keys.index(key))
        values.extend(val)
        offsets.append(len(values))
    out = bytearray([(0 << 2) | 2, len(fields)])
    out.extend(field_ids)
    out.extend(offsets)
    out.extend(values)
    return bytes(out)


keys = ["a", "b"]
obj_meta = meta_with_keys(keys)
obj_value = v_object(keys, [("a", v_int8(1)), ("b", v_short_string("x"))])

rows = [
    {"id": 1, "v": {"metadata": EMPTY_META, "value": v_int8(42)}},
    {"id": 2, "v": {"metadata": EMPTY_META, "value": v_short_string("hello")}},
    {"id": 3, "v": {"metadata": EMPTY_META, "value": v_bool_true()}},
    {"id": 4, "v": {"metadata": obj_meta, "value": obj_value}},
    {"id": 5, "v": None},
]

schema = pa.schema(
    [
        pa.field("id", pa.int32()),
        pa.field(
            "v",
            pa.struct(
                [
                    pa.field("metadata", pa.binary()),
                    pa.field("value", pa.binary()),
                ]
            ),
        ),
    ]
)

table = pa.Table.from_pylist(rows, schema=schema)
pq.write_table(table, OUT_PATH)
print(pq.read_schema(OUT_PATH))
