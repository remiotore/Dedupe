# dedupe

Fast URL dedupelicator for blog-heavy and CMS-heavy bug-bounty URL lists.

It removes static assets and blog-like/CMS noise (not only WordPress),
normalizes URLs, merges query parameters per URL, and optionally upgrades
`http` to `https`. It is a C port of an earlier Python tool, written because
URL-list processing is dominated by parsing and string work that C handles much
faster.

## Build

```sh
gcc -O2 -std=c11 -Wall -Wextra -o dedupe dedupe.c
```

The only external APIs used are POSIX `getline`, `regex`, and `getopt_long`.
It builds on Linux without extra libraries.

## Usage

```sh
./dedupe urls.txt -o clean.txt
cat urls.txt | ./dedupe
./dedupe urls.txt --drop-blog-slugs --exclude '\.(pdf|zip)$'
./dedupe urls.txt --include-statics js,json,xml,txt
./dedupe urls.txt --all-values --stats
```

`FILE` is optional; stdin is used when it is omitted. Input is read fully
before the output file is opened, so `-o` may safely point at the input file.

## Options

| Option | Meaning |
| --- | --- |
| `-o, --output FILE` | Write to `FILE` instead of stdout. |
| `--keep-static` | Keep every static-asset URL. |
| `--include-statics EXT` | Keep URLs ending in these extensions. Repeatable or comma-separated. Leading dots are accepted. |
| `--drop-blog-slugs` | Also drop slug-like paths with at least `--min-hyphens` hyphens. |
| `--min-hyphens N` | Hyphens needed for `--drop-blog-slugs` (default: `4`). |
| `--exclude REGEX` | Extra POSIX ERE drop rule. Repeatable. Matched against the original URL. |
| `--exclude-case-sensitive` | Make `--exclude` case-sensitive (default is case-insensitive). |
| `--keep-p-param` | Do not treat `?p=` as a CMS/blog post query. |
| `--no-prefer-https` | Do not upgrade `http` to `https` when both versions exist. |
| `--keep-tracking` | Keep `utm_*`, `fbclid`, and similar tracking parameters. |
| `--all-values` | Keep every distinct raw value for a repeated key. Default keeps the first value per key. |
| `--sort` | Sort final output. |
| `--stats` | Print input, dropped, and output counts to stderr. |
| `-h, --help` | Show usage. |

`--include-statics` only exempts those extensions from the static-asset drop.
A URL can still be dropped by a blog/CMS rule or `--exclude`.

## Filtering rules

Only `http` and `https` URLs are accepted.

Paths are dropped when they match blog-like/CMS noise:

- WordPress core: `wp-admin`, `wp-content`, `wp-includes`, `wp-json`,
  `wp-login.php`, `wp-cron.php`, `wp-signup.php`, `xmlrpc.php`, `readme.html`,
  `license.txt`
- feeds and comments feeds: `/feed`, `/rss`, `/atom`, `/comments/feed`
- taxonomy pages: `category`, `tag`, `author`, `archive(s)`, `taxonomy`,
  `topic(s)`, `label(s)`
- pagination and comment pages: `/page/N`, `/comment-page-N`
- date archives such as `/2024/`, `/2024/12`, `/2024-12-31`, and dated slugs
- numeric IDs such as `/1234/` and `/1234-slug/`
- blog-like roots: `/blog(s)`, `/weblog(s)`, `/news`, `/newsroom`, `/press`,
  `/post(s)`, `/article(s)`, `/story`, `/stories`, `/journal(s)`, `/note(s)`,
  `/entry`, `/entries`, `/magazine(s)`, `/publication(s)`, `/announcement(s)`,
  `/insight(s)`, `/update(s)`, `/event(s)`
- attachments and revisions

Blog-like roots are matched at the start of the path, so `/blog/post` is
dropped but `/my-blog/post` is not. The filter is deliberately not
WordPress-specific: common CMS, news, magazine, press, and personal-blog
patterns are included.

Hyphenated blog slugs are not dropped by default. Add `--drop-blog-slugs` to
drop long slug-like paths as well, for example `/how-to-do-something-great`.

Static extensions are dropped by default:

```
css js mjs map json xml txt
png jpg jpeg gif webp svg ico bmp tif tiff avif
woff woff2 ttf otf eot
mp3 mp4 wav ogg oga webm avi mov mkv flv m4a m4v
pdf zip rar 7z tar gz bz2 xz
doc docx xls xlsx ppt pptx rtf csv
```

CMS/blog post query parameters (`p`, `page_id`, `attachment_id`,
`preview_id`, `preview_nonce`) drop the URL. `--keep-p-param` exempts only `p`.

Noise parameters (`replytocom`, `cpage`, `calendar`) are always stripped.
Tracking parameters are stripped unless `--keep-tracking` is used:

- prefixes: `utm_`, `mc_`, `mtm_`, `pk_`
- keys: `fbclid`, `gclid`, `gclsrc`, `msclkid`, `yclid`, `dclid`,
  `srsltid`, `_ga`, `_gl`, `_hsenc`, `_hsmi`, `igshid`, `twclid`

## Normalization and merging

A URL is normalized before grouping:

- scheme is lowercased
- host is lowercased; userinfo is preserved as written
- default ports (`:80` for http, `:443` for https) are removed
- IPv6 hosts keep their brackets
- fragments are discarded
- one trailing slash is removed, then repeated slashes are collapsed
- query keys are percent-decoded and lowercased for filtering/comparison
- query values keep their original raw encoding and `key=value` spelling

URLs are grouped by `(host, path)`. The first scheme seen is kept unless
`--no-prefer-https` is given and an `https` version appears later.

By default, the first value for each query key is kept. `--all-values` keeps all
distinct raw `key=value` pairs, in first-seen order.

Example:

```text
http://Example.com:80/a/?utm_source=x&id=1
https://example.com/a?id=2&utm_medium=y#frag
```

becomes:

```text
https://example.com/a?id=1
```

## Limitations

- `--exclude` uses POSIX extended regular expressions, not Python `re`.
  Use `[0-9]` instead of `\d`, and plain groups instead of `(?:...)`.
  Lookarounds and other Python-only syntax are unavailable.
- Percent decoding for matching is byte-oriented. There is no Unicode case
  folding or IDNA/punycode normalization.
- All unique URLs are held in memory, so peak memory grows with the number of
  unique `(host, path)` groups.
- A percent-encoded `NUL` in a path or query key is not supported; C strings
  stop at the first NUL.
- `--include-statics` affects extension filtering only. Blog/CMS path rules
  and `--exclude` still apply.
