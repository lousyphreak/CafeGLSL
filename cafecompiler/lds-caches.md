# Clause-local readonly owner caches on GPU7

GPU7 absolute LDS can be reused by another wave between fetch clauses. The R700
ISA guarantees a non-interruptible exchange when the writes and their reads stay
inside one clause (§2.6.2 and chapter 8). Merely allowing more resident waves is
incorrect for shared contents that survive across clauses.

A proven readonly owner cache now uses that guarantee directly:

1. Keep each lane's captured owner value in its private GPRs.
2. Start every LDS read clause by restoring all owners from those GPRs.
3. Issue up to seven reads after the write in the same eight-instruction clause.
4. Use the read results from private GPRs while other waves reuse physical LDS.

The scheduler inserts the restores before register allocation. Their ordinary
source uses keep the captured owner registers live for the entire cache lifetime.
No shader-side lock, global barrier, CPU synchronization, allocation or data copy
is added. Reads remain masked normally; BROADCAST is not enabled.

## Eligibility

The existing readonly-cache proof establishes a single complete owner store to
one array, a dominating initialization, equal-sized 32-bit reads, affine source
addressing and an immutable nonvolatile/noncoherent buffer source. Consumers stay
inside a uniform cache loop with no intervening global side effects. Replay also
requires native sixteen-byte owner slots and nondivergent read execution, so all
64 owners are active when restoring the cache.

Uniform-address consumers use clause-local LDS. Divergent consumers retain the
existing readonly-buffer forwarding strategy. Packed scalar caches and divergent
read execution use forwarding where its proof holds. Multiple stores/arrays,
partial initialization, transformed source values and intervening writes retain
the conservative native LDS path. Logical groups with LDS still require exactly
64 work items; this does not provide cross-wave barriers or global atomics.

Eligible uniform-address caches use clause-local exchange without a
one-wave residency limit.

## Encoding and allocation

A lowered LDS write carries its native DSW control word: destination slot and
owner stride. The encoder uses this word directly. Stride must remain intact
when a clause-local cache has zero wave-private reservation.

Zero private reservation with local LDS operations selects scheduler replay.
Other LDS programs retain their physical shared size and exclusive residency.
No public API, GFD layout or resource-envelope change is required.
