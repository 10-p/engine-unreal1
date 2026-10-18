#!/usr/bin/env node
// emit-artifact-meta.mjs — write <out>/meta.json for the just-linked WASM artifact.
// Run by the Unreal launcher's POST_BUILD after SDLLaunch.js is final, so the js checksum is of the served
// file. Node stdlib only, so the artifact and its descriptor travel together; the host's build validates it
// against the glue ABI_VERSION and the host loader contract.
//   node emit-artifact-meta.mjs --out=<dir> --target=<id> --abi=<n> --gitSha=<sha> --emcc=<version>
//        [--dep-<label>=<repo>@<ref> ...]
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const args = Object.fromEntries(
    process.argv.slice(2).map((a) => {
        const i = a.indexOf('=');
        return [a.slice(2, i), a.slice(i + 1)];
    }),
);
const dir = args.out;

// Provenance is resolved here, at build time: the values on the POST_BUILD command line are computed once per
// configure, so a plain `cmake --build` would stamp the SHA the tree was configured at. --gitSha is the
// fallback for a tree without git; builtAt is always the time of this run.
const _here = dirname(fileURLToPath(import.meta.url));
const gitSha = (() => {
    try {
        return execFileSync('git', ['-C', _here, 'rev-parse', '--short', 'HEAD'], {
            encoding: 'utf8',
            stdio: ['ignore', 'pipe', 'ignore'],
        }).trim();
    } catch {
        return args.gitSha || 'unknown';
    }
})();
const builtAt = new Date().toISOString().replace(/\.\d+Z$/, 'Z');

const sha = (f) =>
    existsSync(join(dir, f)) ? 'sha256-' + createHash('sha256').update(readFileSync(join(dir, f))).digest('hex') : null;

const checksums = { wasm: sha('SDLLaunch.wasm'), js: sha('SDLLaunch.js') };
const dataSha = sha('SDLLaunch.data'); // optional (only if --preload-file produced it)
if (dataSha) checksums.data = dataSha;

// Every fetched library as `<repo>@<branch|tag>`, passed as --dep-<label>=… (Source/cmake/ArtifactDeps.cmake).
// A GitHub URL is shortened to owner/name; a local checkout override arrives as `local:<path>`, kept verbatim.
const depSpec = (v) => v.replace(/^https:\/\/github\.com\/([^/]+)\/([^/@]+?)(?:\.git)?(@.*)$/, '$1/$2$3');
const deps = Object.fromEntries(
    Object.entries(args)
        .filter(([k]) => k.startsWith('dep-'))
        .map(([k, v]) => [k.slice(4), depSpec(v)]),
);

const meta = {
    targetId: args.target || 'unreal1',
    engineFamily: 'UE1',
    buildConfig: 'wasm-integrated',
    abiVersion: Number(args.abi), // from the vendored frozen ue_abi.h UE_ABI_VERSION
    loaderStyle: 'global-module', // classic <script> + bare globals
    provenance: { repo: 'engine-unreal1', gitSha, builtAt },
    toolchain: { emscripten: args.emcc || 'unknown', base: 'fgsfdsfgs/UE1 (Unreal v200)', ...deps },
    buildFlags: [
    // Descriptive and hand-maintained: keep it in step with the link options in Source/Unreal/CMakeLists.txt.
    // The threading flags make this a threaded-only artifact; the host checks for SharedArrayBuffer and
    // OffscreenCanvas.
    '-sASYNCIFY', '-sFULL_ES2', '-sUSE_SDL=2', '-sALLOW_MEMORY_GROWTH', '-sFORCE_FILESYSTEM', '-sWASMFS',
    '-pthread', '-sPROXY_TO_PTHREAD', '-sOFFSCREENCANVAS_SUPPORT', '-sOFFSCREENCANVASES_TO_PTHREAD=#canvas',
    '-sOFFSCREEN_FRAMEBUFFER', '-sMAX_WEBGL_VERSION=2', '-sMIN_WEBGL_VERSION=1', '-sNO_DISABLE_EXCEPTION_CATCHING',
  ],
    exports: { symbolContract: 1 },
    checksums,
};

writeFileSync(join(dir, 'meta.json'), JSON.stringify(meta, null, 2) + '\n');
console.log(`[meta] wrote ${join(dir, 'meta.json')} (abi ${meta.abiVersion}, ${meta.buildConfig}, ${meta.loaderStyle})`);
