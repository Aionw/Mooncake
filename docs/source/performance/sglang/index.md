# SGLang Performance Benchmarks

Benchmarks evaluating Mooncake's integration with SGLang for PD disaggregation and HiCache.

| Document | Scenario | Key Findings |
|----------|----------|---------------|
| [PD Disaggregation](../sglang-benchmark-results-v1) | 1P1D vs 2 Regular instances on 2 A10 servers | **~30% lower ITL** while maintaining comparable throughput; better TTFT under higher request rates (req=4.0) |
| [HiCache L3 Backend](../sglang-hicache-benchmark-results-v1) | Multi-turn conversation with 4-tier comparison: GPU only → L2 → +Mooncake → pre-populated Mooncake | Pre-populated Mooncake achieves **best TTFT**; as rounds grow, Mooncake maintains high cache hit rate while L2-only drops off sharply |

:::{toctree}
:maxdepth: 1
:hidden:

../sglang-benchmark-results-v1
../sglang-hicache-benchmark-results-v1
:::
