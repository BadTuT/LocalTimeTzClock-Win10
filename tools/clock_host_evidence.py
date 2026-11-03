"""读取本地 DLL 的时钟适配证据；不会加载、执行或修改该模块。"""

import argparse
import bisect
import hashlib
import json
import struct
import uuid
from pathlib import Path

from pe import PE

API_NAMES = (
    "GetTimeFormatEx", "GetDateFormatEx", "GetLocalTime", "GetSystemTime",
    "GetSystemTimeAsFileTime", "SystemTimeToTzSpecificLocalTimeEx",
    "GetDynamicTimeZoneInformation",
)


def pdb_record(pe):
    rva, size = pe.dd(6)
    if not rva or not size:
        return None
    offset = pe.r2o(rva)
    if offset is None or offset + size > len(pe.d):
        raise ValueError("调试目录超出文件范围")
    for i in range(size // 28):
        kind, length, _, pointer = struct.unpack_from("<IIII", pe.d, offset + i * 28 + 12)
        if kind != 2:
            continue
        if length < 25 or pointer + length > len(pe.d):
            raise ValueError("CodeView 记录超出文件范围")
        data = pe.d[pointer:pointer + length]
        if data[:4] != b"RSDS":
            continue
        guid = uuid.UUID(bytes_le=data[4:20]).hex.upper()
        age = struct.unpack_from("<I", data, 20)[0]
        name = data[24:].split(b"\0", 1)[0].decode("utf-8", errors="replace").replace("\\", "/").split("/")[-1]
        return dict(name=name, guid=guid, age=age,
                    symbol_url=f"https://msdl.microsoft.com/download/symbols/{name}/{guid}{age:X}/{name}")
    return None


def collect_evidence(path):
    path = Path(path).resolve()
    raw = path.read_bytes()
    if len(raw) < 0x40 or raw[:2] != b"MZ":
        raise ValueError("输入不是 PE 文件")
    offset = struct.unpack_from("<I", raw, 0x3C)[0]
    if offset + 24 > len(raw) or raw[offset:offset + 4] != b"PE\0\0":
        raise ValueError("PE 签名或文件头无效")
    try:
        pe = PE(str(path))
        if pe.d != raw:
            raise ValueError("采集期间文件发生变化，请重新采集")
        if not pe.is64 or pe.machine != 0x8664:
            raise ValueError("当前工具只分析 x64 PE 文件")
        timestamp = struct.unpack_from("<I", raw, pe.pe + 8)[0]
        import_records = pe.imports() if pe.dd(1)[0] else []
        imports = [dict(module=dll, function=name, iat_rva=slot)
                   for dll, _, name, slot, _ in import_records if name in API_NAMES]
        slots = {item["iat_rva"]: item["function"] for item in imports}
        functions = sorted(pe.pdata())
        starts = [item[0] for item in functions]
        calls = {name: [] for name in API_NAMES}
        for section in pe.sections:
            if section["name"] != ".text":
                continue
            length = min(section["rawsz"], section["vsize"])
            data = raw[section["rawptr"]:section["rawptr"] + length]
            if len(data) != length:
                raise ValueError("代码节超出文件范围")
            for i in range(len(data) - 5):
                if data[i:i + 2] != b"\xff\x15":
                    continue
                call = section["va"] + i
                target = (call + 6 + struct.unpack_from("<i", data, i + 2)[0]) & 0xffffffff
                if target not in slots:
                    continue
                pos = bisect.bisect_right(starts, call) - 1
                function = functions[pos] if pos >= 0 and functions[pos][1] > call else None
                calls[slots[target]].append(dict(
                    call_rva=call, return_rva=call + 6,
                    function_begin_rva=function[0] if function else None,
                    function_end_rva=function[1] if function else None,
                ))
        return dict(
            schema_version=1, module_name=path.name, file_size=len(raw),
            sha256=hashlib.sha256(raw).hexdigest(),
            pe=dict(machine=pe.machine, timestamp=timestamp, size_of_image=pe.sizeofimage),
            imports=imports, call_sites=calls, pdb=pdb_record(pe),
            notice="逐字节调用扫描只提供静态证据，不能证明用途或代替真实任务栏验收。",
        )
    except (IndexError, KeyError, TypeError, struct.error, UnicodeError) as error:
        raise ValueError(f"PE 解析失败：{error}") from error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", type=Path, help="待分析的 x64 DLL 文件")
    parser.add_argument("--out", type=Path, help="将 JSON 写入指定文件；省略则输出到终端")
    args = parser.parse_args()
    try:
        report = collect_evidence(args.module)
        content = json.dumps(report, ensure_ascii=False, indent=2)
        if args.out:
            args.out.write_text(content + "\n", encoding="utf-8")
        else:
            print(content)
    except (OSError, ValueError) as error:
        parser.exit(1, f"采集失败：{error}\n")


if __name__ == "__main__":
    main()
