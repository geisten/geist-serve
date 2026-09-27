# Vendored Markdown lexer

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
