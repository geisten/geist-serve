# Additional 27B catalog artifacts

Metadata checked against publisher Hugging Face API responses on 2026-09-29.
The URLs in `catalog.json` pin immutable repository revisions. File lengths and
SHA-256 values are the publisher's LFS metadata; runtime download verification
still checks the entire file. Both repositories declare Apache-2.0.

| ID | Publisher / artifact | Bytes | Working memory estimate | Total RAM guidance |
| --- | --- | ---: | ---: | ---: |
| `bonsai2-27b-pq2` | prism-ml / Ternary-Bonsai-2-27B-PQ2_0.gguf | 7,206,168,928 | 20 GiB | 24 GiB |
| `qwen38-27b-q4` | unsloth / Qwen3.8-27B-Q4_0.gguf | 16,056,478,688 | 20 GiB | 32 GiB |
| `qwen38-27b-q8` | unsloth / Qwen3.8-27B-Q8_0.gguf | 29,047,086,048 | 34 GiB | 48 GiB |

Working-memory and total-RAM values are conservative planning estimates for the
app's context limit of 4096, not measured RSS, GPU memory or guarantees. Context,
backend buffers, OS use and concurrent applications affect actual requirements.
Q4_0/Q8_0 describe block quantization formats; scales and other tensor types mean
the entire artifact is not exactly four/eight bits per parameter.

## Sources and compatibility

- [Prism model](https://huggingface.co/prism-ml/Ternary-Bonsai-2-27B-gguf), revision
  `b072e1d3b35a0a630cece372c2127528e0994386`.
- [Prism format caveats](https://huggingface.co/prism-ml/Ternary-Bonsai-2-27B-gguf/blob/b072e1d3b35a0a630cece372c2127528e0994386/KNOWN_ISSUES.md):
  PQ2_0/PTQ1_0 require special quantization and Hadamard support. Plain F16 is not
  an interchangeable workaround for an engine lacking the associated metadata.
- [Unsloth Qwen artifacts](https://huggingface.co/unsloth/Qwen3.8-27B-GGUF/tree/4ca720788d1e01f1bff70c033e0d0028fd02e502),
  revision `4ca720788d1e01f1bff70c033e0d0028fd02e502`.
- The pinned geistlib commit `e26436906ff6fe7eda296b90fa3a7a9dfa69f418`
  implements the Qwen hybrid architecture, Q4_0/Q8_0 and Bonsai PQ2_0 with Prism
  Hadamard transforms. Catalog revision 3 enables Bonsai on CPU and Metal.
  Real CPU NEON and Metal inference and the upstream reference-token test were
  verified on an Apple M1 Max. Linux/x86 and Vulkan were not exercised by that
  check. The packaged Mac runtime includes Metal; the catalog does not advertise
  Vulkan for Bonsai. Engine and catalog changes must be shipped together.

Bonsai's previous 12 GiB working estimate was below the observed 13.6 GiB CPU
process RSS. The revised 20 GiB working budget and 24 GiB total-RAM guidance add
planning reserve; they are not measured peaks or guarantees for every prompt.
For now the same allowance applies to CPU and Metal. Process RSS does not include
all Metal allocations and cannot justify a lower GPU budget. Separate budgets
require complete backend-memory and longer-context measurements.

The engine header still reports 0.11.0, as did the previous unsupported commit
`25861c0bd197f1a98f17e49efe0cdc48a0e40713`. Use the pinned commit to identify
Bonsai support, not the semantic version alone.

The catalog is configuration, not a benchmark or an answer-quality approval.
Adding entries does not establish 27B end-to-end acceptance. Test evidence and
any remaining inference limitations are recorded with the application change.
