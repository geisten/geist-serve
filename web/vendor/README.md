# Vendored Markdown and math parsers

Marked 18.0.14 is included from the official npm `marked` package. The tarball
was checked against its published SHA-512 integrity value; `manifest.json`
records that value and SHA-256 values of the exact shipped source and license.
`tests/app/http_test.py` checks these hashes and embedded asset bytes in CI.

Only `marked.lexer()` is used. `web/markdown.js` builds an allowlisted DOM tree
and never uses Marked’s HTML renderer. Raw HTML is literal text. Links expose
copyable HTTP(S) addresses, not navigation. Images are not fetched. No CDN,
remote fonts, runtime package manager, or syntax-highlighting service is needed.
The MIT license is included in Mac, Debian and portable packages.

When updating, review upstream changes/security advisories, re-pin the exact
package, verify integrity, and run native WebView security and streaming tests
on both platforms. Do not edit the vendored parser in place.

Upstream: https://github.com/markedjs/marked
Lexer API: https://marked.js.org/using_pro#lexer

## Math

KaTeX 0.18.9 is pinned in `katex-manifest.json`. Its official npm tarball was
verified against the published SHA-512 integrity value. Only the unmodified
browser renderer and MIT license are shipped. The same HTTP asset/integrity
test covers both parsers.

The application uses `katex.render` with `output: 'mathml'`: native MathML
provides visual layout and mathematical semantics without remote resources,
custom fonts, a CDN or KaTeX's HTML-layout CSS. The existing CSP stays intact.
`trust: false`, strict parsing, bounded expansion/size/input/count/nesting and
fresh macro state per formula restrict untrusted output. Invalid expressions
fall back to literal text; parser errors are never inserted as HTML.

Inline `$…$` / `\(…\)` and display `$$…$$` / `\[…\]` expressions are tokenized
before Markdown consumes their escapes. Code and escaped dollars stay literal;
common `$5 and $10` currency pairs are not formulas. An unclosed expression is
visible while streaming, and becomes MathML once complete. Dollar notation is
inherently ambiguous: use `\(…\)` for unambiguous math, or escape literal dollars.
Completed formulas are cached per live message and their DOM is reconciled to
preserve focus/scroll. Copying the complete reply retains the original source.

Supported expressions are KaTeX's math subset, not arbitrary LaTeX documents.
Native MathML layout is checked in macOS WKWebView; the shared desktop checks
also run in the Linux WebKit pipeline. Limits and unsupported commands preserve
source rather than silently omitting information.

Upstream: https://github.com/KaTeX/KaTeX
Options: https://katex.org/docs/options
Security: https://katex.org/docs/security
