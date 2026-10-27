// Opt-in, offline Khronos cross-check of Shark's licensed glTF fixtures.
// Preparation and tool provenance: docs/dependencies/gltf-validator.md.
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

const root = path.resolve(__dirname, '..');
const version = '2.0.0-dev.3.10';
const hashes = {
    'index.js': '78deff9ea85743e86461c2d14fae76e7fc3ca0432e652f62948066b55fa16f0d',
    'gltf_validator.dart.js': 'b73a7b2d455ac217567725138b46d826a13d7d1bb0c88c15f7c571bfb349298c',
    'package.json': '3578d16153fa237c72784588da2e8fcccc9cd3c1c58ef34c7912f0732d2f5fa6',
    'LICENSE': '3ddf9be5c28fe27dad143a5dc76eea25222ad1dd68934a047064e56ed2fa40c5',
    'NOTICES': '933f161ca1e7b3ead5a6cf93ebb3bf6cb67f0e79b38e08d2046f8cdc41cb78a3',
};
const corpus = {
    gltf_source: ['minimal.gltf', 'external.gltf', 'embedded.gltf', 'triangle.glb',
                  'sparse.gltf', 'repaired-indices.gltf'],
    gltf_geometry: ['shared-strip.gltf', 'embedded-strip.gltf', 'shared-strip.glb'],
    gltf_textures: ['external.gltf', 'embedded.gltf', 'embedded.glb'],
};

function requireThat(condition, message) {
    if (!condition) throw new Error(message);
}
function digest(bytes) {
    return crypto.createHash('sha256').update(bytes).digest('hex');
}
function read(file) {
    requireThat(fs.statSync(file).isFile() && fs.statSync(file).size <= 1024 * 1024,
                `Missing, nonregular or oversized oracle input: ${file}`);
    // Dart's JS external-resource bridge expects a zero-offset ArrayBuffer.
    // A pooled Node Buffer may expose unrelated bytes before its byteOffset.
    return new Uint8Array(fs.readFileSync(file));
}
function resourceReader(directory, missingImage = false) {
    const boundary = fs.realpathSync(directory);
    return async (uri) => {
        requireThat(!/[:\\?#]/.test(uri), `Nonlocal oracle URI: ${uri}`);
        const decoded = decodeURIComponent(uri);
        requireThat(!/[:\\?#\0]/.test(decoded), `Invalid oracle URI: ${uri}`);
        if (missingImage && decoded.endsWith('.png')) throw new Error('Deliberately missing image');
        const resolved = fs.realpathSync(path.resolve(boundary, decoded));
        const relative = path.relative(boundary, resolved);
        requireThat(relative !== '..' && !relative.startsWith(`..${path.sep}`) &&
                    !path.isAbsolute(relative), `Oracle resource leaves fixture: ${uri}`);
        return read(resolved);
    };
}

async function main() {
    requireThat(process.argv.length === 4,
                'Usage: node tools/check_gltf_oracle.cjs <prepared-package-directory> <output-directory>');
    const packageDirectory = path.resolve(process.argv[2]);
    const outputDirectory = path.resolve(process.argv[3]);
    for (const [name, expected] of Object.entries(hashes)) {
        requireThat(digest(read(path.join(packageDirectory, name))) === expected,
                    `Pinned validator input differs: ${name}`);
    }
    const validator = require(path.join(packageDirectory, 'index.js'));
    requireThat(validator.version() === version, 'Unexpected Khronos validator version');
    fs.mkdirSync(outputDirectory, {recursive: true});
    const results = [];
    const check = async (name, bytes, directory, expectedError = null, missingImage = false) => {
        const report = await validator.validateBytes(bytes, {
            uri: name, writeTimestamp: false, maxIssues: 100,
            externalResourceFunction: resourceReader(directory, missingImage),
        });
        const errors = report.issues.messages.filter(message => message.severity === 0);
        const warnings = report.issues.messages.filter(message => message.severity === 1);
        const passed = !report.issues.truncated && (expectedError
            ? errors.some(message => message.code === expectedError)
            : report.issues.numErrors === 0 && warnings.every(message =>
                message.code === 'MESH_PRIMITIVE_GENERATED_TANGENT_SPACE'));
        fs.writeFileSync(path.join(outputDirectory, `${name}.json`), JSON.stringify(report, null, 2) + '\n');
        results.push({name, sha256: digest(bytes), expected_error: expectedError,
                      errors: report.issues.numErrors, warnings: report.issues.numWarnings,
                      status: passed ? 'PASS' : 'FAIL'});
        console.log(`${passed ? 'PASS' : 'FAIL'} ${name}: ${report.issues.numErrors} errors, ${report.issues.numWarnings} warnings`);
    };
    for (const [directory, names] of Object.entries(corpus)) {
        const fixtureDirectory = path.join(root, 'tests', 'fixtures', directory);
        for (const name of names) {
            await check(`${directory}-${name}`, read(path.join(fixtureDirectory, name)), fixtureDirectory);
        }
    }
    const sourceDirectory = path.join(root, 'tests', 'fixtures', 'gltf_source');
    const malformed = JSON.parse(fs.readFileSync(path.join(sourceDirectory, 'external.gltf'), 'utf8'));
    malformed.accessors[0].count = 4;
    await check('negative-accessor-bounds.gltf', new TextEncoder().encode(JSON.stringify(malformed)),
                sourceDirectory, 'ACCESSOR_TOO_LONG');
    const texturesDirectory = path.join(root, 'tests', 'fixtures', 'gltf_textures');
    await check('negative-missing-image.gltf', read(path.join(texturesDirectory, 'external.gltf')),
                texturesDirectory, 'IO_ERROR', true);
    const invalidGlb = read(path.join(sourceDirectory, 'triangle.glb'));
    new DataView(invalidGlb.buffer).setUint32(8, invalidGlb.length + 4, true);
    await check('negative-container-length.glb', invalidGlb, sourceDirectory, 'GLB_LENGTH_MISMATCH');
    const passed = results.every(result => result.status === 'PASS');
    fs.writeFileSync(path.join(outputDirectory, 'summary.json'), JSON.stringify({
        schema_version: 1, validator: version, node: process.version,
        scope: 'Independent offline fixture cross-check; not engine or rendering acceptance',
        status: passed ? 'PASS' : 'FAIL', results,
    }, null, 2) + '\n');
    process.exitCode = passed ? 0 : 1;
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
