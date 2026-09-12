# Wikipedia StarDict exporter

This desktop-side tool converts DBpedia's extracted Wikipedia article abstracts
into a plain-text StarDict supported by CrossPoint Reader. It produces one
definition per Wikipedia article title. It does not contain complete articles,
images, CSS, or JavaScript.

The exporter writes StarDict directly and uses only the Python standard library.
An on-disk SQLite database performs the sort, so the complete dataset is never
held in RAM.

## Input

Pass a DBpedia `short-abstracts` Turtle/N-Triples file, compressed with bzip2 or
gzip or uncompressed. A URL can be passed directly and is downloaded into
`.cache/wikipedia-stardict/`, with interrupted downloads resumed when the server
supports HTTP ranges.

For example, this is DBpedia's dated English 2021.06.01 dataset:

```powershell
py -3 scripts/wikipedia_stardict/build.py `
  --abstracts "https://downloads.dbpedia.org/repo/dbpedia/text/short-abstracts/2021.06.01/short-abstracts_lang=en.ttl.bz2" `
  --output build/wikipedia-stardict-en
```

DBpedia publishes newer artifacts through its Databus. Pass the direct URL of a
newer `short-abstracts_lang=en.ttl.bz2` file when one is available. The exporter
does not silently select a changing dataset so builds remain reproducible.

To add Wikipedia redirects as alternate headwords, also supply the matching
DBpedia redirects dump:

```powershell
py -3 scripts/wikipedia_stardict/build.py `
  --abstracts "C:\data\short-abstracts_lang=en.ttl.bz2" `
  --redirects "C:\data\redirects_lang=en.ttl.bz2" `
  --output build/wikipedia-stardict-en
```

On Linux or WSL, use the same arguments with `python3` and normal shell line
continuations.

## Smoke test

Before processing the complete dump, build a small dictionary:

```powershell
py -3 scripts/wikipedia_stardict/build.py `
  --abstracts "C:\data\short-abstracts_lang=en.ttl.bz2" `
  --output build/wikipedia-stardict-test `
  --stem wikipedia-test `
  --name "Wikipedia Test" `
  --max-entries 10000
```

The output contains:

- `wikipedia-en.ifo`
- `wikipedia-en.idx` (uncompressed, as required by CrossPoint Reader)
- `wikipedia-en.dict`
- `wikipedia-en.syn` when `--redirects` was provided

Copy the entire output directory to `/dictionaries/wikipedia-en/` on the SD
card, select it in **Settings -> Reader -> Dictionary**, and look up a known
article title. The first lookup builds the firmware's sampled index sidecars.

## Compatibility limits

- Headwords longer than 255 UTF-8 bytes are skipped.
- Definitions are normalized to one paragraph and capped at 64 KiB.
- The exporter aborts rather than emitting `idxoffsetbits=64` if definition data
  exceeds the 32-bit StarDict address space supported by CrossPoint Reader.
- Existing output files are preserved unless `--force` is passed.
- Output is deliberately uncompressed. CrossPoint Reader supports `.dict`
  directly, and this avoids requiring `dictzip` or another host dependency.

Use `--help` for the complete option list.

## Licensing

DBpedia is derived from Wikipedia. DBpedia documents releases from version 3.4
onward as dual-licensed under CC BY-SA 3.0 and the GNU Free Documentation
License. The generated `.ifo` retains a textual DBpedia attribution. Preserve
that metadata if you redistribute the resulting dictionary.
