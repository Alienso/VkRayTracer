# Clouds (VOLUMES_PLAN.md step 3), set aside

Reverted from the working tree on 2026-10-12, kept here in case we return to it.

- `clouds-step3.patch`: the step-3 changes on top of steps 1-2 (apply from the repo root: `git apply archive/clouds-step3/clouds-step3.patch`).
- `current/`: the full files as they were with clouds in (shader included), and the plan with the step-3 results.

What it contained:

- A separate scatter budget: fog scatter points don't count as bounces ("Max fog scatters", default 1024).
- A cloud phase function: the approximate Mie function of Jendersie & d'Eon 2023 ("Cloud droplets", droplet size). By default
  the narrow forward peak is folded into the extinction (delta scaling); "Exact forward peak" traces it.
- "Edge detail": the noise read 4x finer erodes the thin parts of the blobs.
- A built-in "Cloud" model.
- A fix: the direction is normalized at each fog scatter point (after hundreds of scatters, NaN otherwise).

Why it was set aside: too slow. The default cloud was 16 ms of trace time in DLAA (65 ms at thickness 32). The point-light
Cornell box full of fog went from 22 to 58 ms, because paths no longer stop at Max bounces in fog.
