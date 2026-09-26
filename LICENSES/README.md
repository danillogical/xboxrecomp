# Third-party licence texts

Two licence texts live here, and the top-level NOTICE says which files each one
governs. Neither is a courtesy copy: shipping the verbatim text alongside the
files is a condition of the licence.

## `GPL-2.0.txt`

The verbatim GNU General Public License v2, from

    https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt

It applies to the files listed under "GPL-2.0-or-later" in the NOTICE — the
DSP56300 core and interpreter extracted from xemu at the pinned commit
`67cc79e663038d1f55448c0f566b37dde016adf6`, under `src/apu/dsp/`.

These are the **strongest** terms in this repository. Because they are linked
into `xbox_apu`, a binary that links it is a **GPL-2.0-or-later combined work**:
it must be distributed under the GPL, and the LGPL's relinking permission does
**not** apply to it. The project's own MIT licence covers its own code only.

## `LGPL-2.1.txt`

The verbatim GNU Lesser General Public License v2.1, from

    https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt

It applies to the files listed under "LGPL-2.1-or-later" in the NOTICE — the
MCPX APU sources and `src/nv2a/nv2a_regs.h`, extracted from xemu and remaining
the copyright of espes, Jannik Vogel and Matt Borgerson.

Linking against these from MIT or proprietary code is expressly permitted by
the LGPL. What it asks in return is that the notices stay, that the source stays
available, and that a user can relink against a modified version.
