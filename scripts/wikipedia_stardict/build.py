#!/usr/bin/env python3
"""Build an X4-compatible StarDict from DBpedia Wikipedia abstracts."""

from __future__ import annotations

import argparse
import bz2
import gzip
import os
import re
import sqlite3
import struct
import sys
import time
import urllib.parse
import urllib.request
from contextlib import closing
from datetime import UTC, datetime
from pathlib import Path
from typing import BinaryIO, Iterator, TextIO


ABSTRACT_PREDICATES = {
    "http://dbpedia.org/ontology/abstract",
    "http://www.w3.org/2000/01/rdf-schema#comment",
}
REDIRECT_PREDICATE = "http://dbpedia.org/ontology/wikiPageRedirects"
RESOURCE_PREFIXES = (
    "http://dbpedia.org/resource/",
    "https://dbpedia.org/resource/",
)
MAX_STARDICT_OFFSET = 0xFFFFFFFF
MAX_HEADWORD_BYTES = 255
DEFAULT_MAX_DEFINITION_BYTES = 64 * 1024
INSERT_BATCH_SIZE = 2_000
PROGRESS_INTERVAL = 100_000

LITERAL_TRIPLE_RE = re.compile(
    r'^<([^>]*)>\s+<([^>]*)>\s+"(.*)"@([A-Za-z0-9-]+)\s*\.\s*$'
)
IRI_TRIPLE_RE = re.compile(r"^<([^>]*)>\s+<([^>]*)>\s+<([^>]*)>\s*\.\s*$")


class BuildError(RuntimeError):
    """An expected input or compatibility failure."""


class Progress:
    def __init__(self, label: str) -> None:
        self.label = label
        self.started = time.monotonic()
        self.last_report = self.started

    def report(self, scanned: int, accepted: int, force: bool = False) -> None:
        now = time.monotonic()
        if not force and scanned % PROGRESS_INTERVAL != 0 and now - self.last_report < 10:
            return
        elapsed = max(now - self.started, 0.001)
        print(
            f"{self.label}: scanned {scanned:,}, accepted {accepted:,} "
            f"({scanned / elapsed:,.0f} lines/s)",
            file=sys.stderr,
        )
        self.last_report = now


def ascii_fold(data: bytes) -> bytes:
    """Match the firmware's byte-wise, ASCII-only case-insensitive ordering."""
    return data.translate(bytes.maketrans(b"ABCDEFGHIJKLMNOPQRSTUVWXYZ", b"abcdefghijklmnopqrstuvwxyz"))


def decode_ntriples_escapes(value: str) -> str:
    result: list[str] = []
    index = 0
    simple = {
        "t": "\t",
        "b": "\b",
        "n": "\n",
        "r": "\r",
        "f": "\f",
        '"': '"',
        "'": "'",
        "\\": "\\",
    }
    while index < len(value):
        char = value[index]
        if char != "\\":
            result.append(char)
            index += 1
            continue
        if index + 1 >= len(value):
            raise ValueError("trailing backslash")
        escape = value[index + 1]
        if escape in simple:
            result.append(simple[escape])
            index += 2
            continue
        if escape in ("u", "U"):
            digits = 4 if escape == "u" else 8
            start = index + 2
            end = start + digits
            if end > len(value):
                raise ValueError("short Unicode escape")
            codepoint = int(value[start:end], 16)
            result.append(chr(codepoint))
            index = end
            continue
        raise ValueError(f"unknown escape \\{escape}")
    return "".join(result)


def title_from_resource(resource: str) -> str | None:
    for prefix in RESOURCE_PREFIXES:
        if resource.startswith(prefix):
            encoded_title = resource[len(prefix) :]
            try:
                encoded_title = decode_ntriples_escapes(encoded_title)
            except (ValueError, OverflowError):
                return None
            return urllib.parse.unquote(encoded_title).replace("_", " ").strip()
    return None


def normalize_definition(definition: str, max_bytes: int) -> bytes:
    definition = " ".join(definition.split())
    encoded = definition.encode("utf-8")
    if len(encoded) <= max_bytes:
        return encoded
    return encoded[:max_bytes].decode("utf-8", errors="ignore").encode("utf-8")


def open_text_source(path: Path) -> TextIO:
    suffix = path.suffix.lower()
    if suffix == ".bz2":
        return bz2.open(path, "rt", encoding="utf-8", errors="strict")
    if suffix == ".gz":
        return gzip.open(path, "rt", encoding="utf-8", errors="strict")
    return path.open("rt", encoding="utf-8", errors="strict")


def download(url: str, cache_dir: Path, redownload: bool) -> Path:
    cache_dir.mkdir(parents=True, exist_ok=True)
    filename = Path(urllib.parse.unquote(urllib.parse.urlparse(url).path)).name
    if not filename:
        raise BuildError(f"Cannot determine a filename from URL: {url}")
    destination = cache_dir / filename
    partial = destination.with_name(destination.name + ".part")
    if destination.exists() and not redownload:
        print(f"Using cached download: {destination}", file=sys.stderr)
        return destination

    existing = partial.stat().st_size if partial.exists() and not redownload else 0
    if redownload and partial.exists():
        partial.unlink()
    request = urllib.request.Request(url, headers={"User-Agent": "crosspoint-wikipedia-stardict/1"})
    if existing:
        request.add_header("Range", f"bytes={existing}-")

    print(f"Downloading {url}", file=sys.stderr)
    with closing(urllib.request.urlopen(request)) as response:
        append = existing > 0 and response.status == 206
        mode = "ab" if append else "wb"
        downloaded = existing if append else 0
        total_header = response.headers.get("Content-Length")
        total = downloaded + int(total_header) if total_header else None
        with partial.open(mode) as output:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                output.write(chunk)
                downloaded += len(chunk)
                if total:
                    print(
                        f"\rDownloaded {downloaded / (1024 * 1024):,.1f} / "
                        f"{total / (1024 * 1024):,.1f} MiB",
                        end="",
                        file=sys.stderr,
                    )
        if total:
            print(file=sys.stderr)
    partial.replace(destination)
    return destination


def resolve_source(value: str, cache_dir: Path, redownload: bool) -> Path:
    parsed = urllib.parse.urlparse(value)
    if parsed.scheme in ("http", "https"):
        return download(value, cache_dir, redownload)
    path = Path(value).expanduser().resolve()
    if not path.is_file():
        raise BuildError(f"Source file does not exist: {path}")
    return path


def configure_database(path: Path) -> sqlite3.Connection:
    connection = sqlite3.connect(path)
    connection.execute("PRAGMA journal_mode=OFF")
    connection.execute("PRAGMA synchronous=OFF")
    connection.execute("PRAGMA temp_store=FILE")
    connection.execute("PRAGMA locking_mode=EXCLUSIVE")
    connection.execute("PRAGMA cache_size=-65536")
    connection.execute(
        "CREATE TABLE entries ("
        "sort_key BLOB PRIMARY KEY, subject TEXT NOT NULL, "
        "headword BLOB NOT NULL, definition BLOB NOT NULL) WITHOUT ROWID"
    )
    return connection


def iter_abstracts(
    source: Path, language: str, max_definition_bytes: int
) -> Iterator[tuple[bytes, str, bytes, bytes]]:
    with open_text_source(source) as input_file:
        for line in input_file:
            match = LITERAL_TRIPLE_RE.match(line.rstrip("\r\n"))
            if not match:
                continue
            subject, predicate, literal, literal_language = match.groups()
            if predicate not in ABSTRACT_PREDICATES or literal_language.lower() != language.lower():
                continue
            title = title_from_resource(subject)
            if not title or "\0" in title:
                continue
            try:
                definition = decode_ntriples_escapes(literal)
            except (ValueError, OverflowError):
                continue
            headword = title.encode("utf-8")
            if len(headword) > MAX_HEADWORD_BYTES:
                continue
            definition_bytes = normalize_definition(definition, max_definition_bytes)
            if not definition_bytes:
                continue
            yield ascii_fold(headword), subject, headword, definition_bytes


def ingest_abstracts(
    connection: sqlite3.Connection,
    source: Path,
    language: str,
    max_definition_bytes: int,
    max_entries: int | None,
) -> int:
    progress = Progress("Abstracts")
    batch: list[tuple[bytes, str, bytes, bytes]] = []
    scanned = 0
    accepted = 0
    connection.execute("BEGIN")
    for record in iter_abstracts(source, language, max_definition_bytes):
        scanned += 1
        batch.append(record)
        if len(batch) >= INSERT_BATCH_SIZE:
            connection.executemany("INSERT OR IGNORE INTO entries VALUES (?, ?, ?, ?)", batch)
            accepted += len(batch)
            batch.clear()
        progress.report(scanned, accepted)
        if max_entries is not None and scanned >= max_entries:
            break
    if batch:
        connection.executemany("INSERT OR IGNORE INTO entries VALUES (?, ?, ?, ?)", batch)
        accepted += len(batch)
    connection.commit()
    unique_count = connection.execute("SELECT count(*) FROM entries").fetchone()[0]
    progress.report(scanned, unique_count, force=True)
    if unique_count == 0:
        raise BuildError(
            "No abstracts were parsed. Check that the input is a DBpedia short-abstracts "
            f"N-Triples/Turtle file containing @{language} literals."
        )
    return unique_count


def write_dictionary(
    connection: sqlite3.Connection,
    output_dir: Path,
    stem: str,
    bookname: str,
    source_description: str,
    write_ordinals: bool,
) -> tuple[int, int]:
    output_dir.mkdir(parents=True, exist_ok=True)
    final_dict = output_dir / f"{stem}.dict"
    final_idx = output_dir / f"{stem}.idx"
    final_ifo = output_dir / f"{stem}.ifo"

    temp_dict = output_dir / f".{stem}.dict.tmp"
    temp_idx = output_dir / f".{stem}.idx.tmp"
    temp_ifo = output_dir / f".{stem}.ifo.tmp"
    for path in (temp_dict, temp_idx, temp_ifo):
        if path.exists():
            path.unlink()

    if write_ordinals:
        connection.execute(
            "CREATE TABLE ordinals (subject TEXT PRIMARY KEY, ordinal INTEGER NOT NULL) WITHOUT ROWID"
        )
    ordinal_batch: list[tuple[str, int]] = []
    offset = 0
    count = 0
    previous_key: bytes | None = None
    try:
        with temp_dict.open("wb") as dictionary_file, temp_idx.open("wb") as index_file:
            cursor = connection.execute(
                "SELECT sort_key, subject, headword, definition FROM entries ORDER BY sort_key"
            )
            if write_ordinals:
                connection.execute("BEGIN")
            for sort_key, subject, headword, definition in cursor:
                if previous_key is not None and sort_key < previous_key:
                    raise BuildError("Internal sort order failure")
                if offset + len(definition) > MAX_STARDICT_OFFSET:
                    raise BuildError(
                        "Definitions exceed StarDict's 32-bit 4 GiB address space. "
                        "Use a smaller source or split the dataset."
                    )
                dictionary_file.write(definition)
                index_file.write(headword)
                index_file.write(b"\0")
                index_file.write(struct.pack(">II", offset, len(definition)))
                if write_ordinals:
                    ordinal_batch.append((subject, count))
                if write_ordinals and len(ordinal_batch) >= INSERT_BATCH_SIZE:
                    connection.executemany("INSERT INTO ordinals VALUES (?, ?)", ordinal_batch)
                    ordinal_batch.clear()
                offset += len(definition)
                count += 1
                previous_key = sort_key
                if count % PROGRESS_INTERVAL == 0:
                    print(f"Dictionary: wrote {count:,} entries", file=sys.stderr)
            if write_ordinals and ordinal_batch:
                connection.executemany("INSERT INTO ordinals VALUES (?, ?)", ordinal_batch)
            if write_ordinals:
                connection.commit()

        idx_size = temp_idx.stat().st_size
        if idx_size > MAX_STARDICT_OFFSET:
            raise BuildError("The .idx exceeds the firmware's 32-bit file-size limit")
        ifo = (
            "StarDict's dict ifo file\n"
            "version=2.4.2\n"
            f"wordcount={count}\n"
            f"idxfilesize={idx_size}\n"
            f"bookname={bookname}\n"
            f"description={source_description}\n"
            f"date={datetime.now(UTC).date().isoformat()}\n"
            "sametypesequence=m\n"
        )
        temp_ifo.write_text(ifo, encoding="utf-8", newline="\n")
        os.replace(temp_dict, final_dict)
        os.replace(temp_idx, final_idx)
        os.replace(temp_ifo, final_ifo)
    except Exception:
        for path in (temp_dict, temp_idx, temp_ifo):
            if path.exists():
                path.unlink()
        raise
    return count, offset


def iter_redirects(source: Path) -> Iterator[tuple[bytes, bytes, str]]:
    with open_text_source(source) as input_file:
        for line in input_file:
            match = IRI_TRIPLE_RE.match(line.rstrip("\r\n"))
            if not match:
                continue
            subject, predicate, target = match.groups()
            if predicate != REDIRECT_PREDICATE:
                continue
            title = title_from_resource(subject)
            if not title or "\0" in title:
                continue
            headword = title.encode("utf-8")
            if len(headword) > MAX_HEADWORD_BYTES:
                continue
            yield ascii_fold(headword), headword, target


def write_synonyms(
    connection: sqlite3.Connection,
    redirects_source: Path,
    output_dir: Path,
    stem: str,
) -> int:
    connection.execute(
        "CREATE TABLE redirects ("
        "sort_key BLOB PRIMARY KEY, headword BLOB NOT NULL, target TEXT NOT NULL) WITHOUT ROWID"
    )
    progress = Progress("Redirects")
    batch: list[tuple[bytes, bytes, str]] = []
    scanned = 0
    connection.execute("BEGIN")
    for record in iter_redirects(redirects_source):
        scanned += 1
        batch.append(record)
        if len(batch) >= INSERT_BATCH_SIZE:
            connection.executemany("INSERT OR IGNORE INTO redirects VALUES (?, ?, ?)", batch)
            batch.clear()
        progress.report(scanned, scanned)
    if batch:
        connection.executemany("INSERT OR IGNORE INTO redirects VALUES (?, ?, ?)", batch)
    connection.commit()

    final_syn = output_dir / f"{stem}.syn"
    temp_syn = output_dir / f".{stem}.syn.tmp"
    if temp_syn.exists():
        temp_syn.unlink()

    count = 0
    try:
        with temp_syn.open("wb") as synonym_file:
            cursor = connection.execute(
                "SELECT redirects.headword, ordinals.ordinal "
                "FROM redirects JOIN ordinals ON redirects.target = ordinals.subject "
                "LEFT JOIN entries ON redirects.sort_key = entries.sort_key "
                "WHERE entries.sort_key IS NULL ORDER BY redirects.sort_key"
            )
            for headword, ordinal in cursor:
                synonym_file.write(headword)
                synonym_file.write(b"\0")
                synonym_file.write(struct.pack(">I", ordinal))
                count += 1
                if count % PROGRESS_INTERVAL == 0:
                    print(f"Synonyms: wrote {count:,} entries", file=sys.stderr)
        os.replace(temp_syn, final_syn)
    except Exception:
        if temp_syn.exists():
            temp_syn.unlink()
        raise

    ifo_path = output_dir / f"{stem}.ifo"
    lines = ifo_path.read_text(encoding="utf-8").splitlines()
    lines.insert(3, f"synwordcount={count}")
    ifo_path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    progress.report(scanned, count, force=True)
    return count


def positive_int(value: str) -> int:
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return number


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Convert a DBpedia Wikipedia short-abstracts dump into a plain-text, "
            "32-bit StarDict supported by CrossPoint Reader."
        )
    )
    parser.add_argument("--abstracts", required=True, help="DBpedia .ttl[.bz2/.gz] file or URL")
    parser.add_argument("--redirects", help="Optional DBpedia redirects .ttl[.bz2/.gz] file or URL")
    parser.add_argument("--language", default="en", help="Literal language tag to retain (default: en)")
    parser.add_argument(
        "--output", type=Path, default=Path("build/wikipedia-stardict-en"), help="Output dictionary directory"
    )
    parser.add_argument("--stem", default="wikipedia-en", help="Output filename stem")
    parser.add_argument("--name", default="English Wikipedia Abstracts", help="StarDict display name")
    parser.add_argument(
        "--cache-dir", type=Path, default=Path(".cache/wikipedia-stardict"), help="Download/work cache"
    )
    parser.add_argument(
        "--max-definition-bytes",
        type=positive_int,
        default=DEFAULT_MAX_DEFINITION_BYTES,
        help=f"Per-entry UTF-8 limit (default: {DEFAULT_MAX_DEFINITION_BYTES})",
    )
    parser.add_argument("--max-entries", type=positive_int, help="Build only N entries for a smoke test")
    parser.add_argument("--redownload", action="store_true", help="Replace cached source downloads")
    parser.add_argument("--keep-work-db", action="store_true", help="Keep the temporary SQLite database")
    parser.add_argument("--force", action="store_true", help="Replace existing output files")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.max_definition_bytes > DEFAULT_MAX_DEFINITION_BYTES:
        raise BuildError(
            f"--max-definition-bytes cannot exceed the firmware limit of {DEFAULT_MAX_DEFINITION_BYTES}"
        )
    if not re.fullmatch(r"[A-Za-z0-9._-]+", args.stem):
        raise BuildError("--stem may contain only letters, numbers, dot, underscore, and hyphen")

    cache_dir = args.cache_dir.expanduser().resolve()
    output_dir = args.output.expanduser().resolve()
    expected_outputs = [
        output_dir / f"{args.stem}.ifo",
        output_dir / f"{args.stem}.idx",
        output_dir / f"{args.stem}.dict",
        output_dir / f"{args.stem}.syn",
    ]
    existing_outputs = [path for path in expected_outputs if path.exists()]
    if existing_outputs and not args.force:
        names = ", ".join(path.name for path in existing_outputs)
        raise BuildError(f"Output exists; pass --force to replace it: {names}")
    cache_dir.mkdir(parents=True, exist_ok=True)
    abstracts_source = resolve_source(args.abstracts, cache_dir, args.redownload)
    redirects_source = (
        resolve_source(args.redirects, cache_dir, args.redownload) if args.redirects else None
    )
    work_db = cache_dir / f"{args.stem}.sqlite3"
    if work_db.exists():
        work_db.unlink()

    connection = configure_database(work_db)
    try:
        unique_count = ingest_abstracts(
            connection,
            abstracts_source,
            args.language,
            args.max_definition_bytes,
            args.max_entries,
        )
        description = (
            f"Wikipedia {args.language} short abstracts derived from DBpedia "
            "(https://dbpedia.org/); licensed under CC BY-SA 3.0 and GFDL."
        )
        count, definition_bytes = write_dictionary(
            connection,
            output_dir,
            args.stem,
            args.name,
            description,
            write_ordinals=redirects_source is not None,
        )
        synonym_count = 0
        stale_synonym_file = output_dir / f"{args.stem}.syn"
        if stale_synonym_file.exists():
            stale_synonym_file.unlink()
        if redirects_source:
            synonym_count = write_synonyms(
                connection, redirects_source, output_dir, args.stem
            )
    finally:
        connection.close()
        if work_db.exists() and not args.keep_work_db:
            work_db.unlink()

    print(f"Built {output_dir}")
    print(f"Entries: {count:,} ({unique_count:,} unique source headwords)")
    print(f"Definition data: {definition_bytes / (1024 * 1024):,.1f} MiB")
    if redirects_source:
        print(f"Redirect synonyms: {synonym_count:,}")
    print(f"Copy this directory to /dictionaries/{args.stem}/ on the SD card.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (BuildError, OSError, sqlite3.Error, UnicodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
