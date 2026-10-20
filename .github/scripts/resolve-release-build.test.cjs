const { test } = require('node:test');
const assert = require('node:assert/strict');
const { hasPackages, resolveBuild, resolveReleaseBuild } = require('./resolve-release-build.cjs');

const sha = 'a'.repeat(40);
function run(overrides = {}) {
    return { id: 10, head_sha: sha, head_branch: 'main', event: 'push',
        status: 'completed', conclusion: 'success', ...overrides };
}
function packages() {
    return ['fastpdf-windows-x64']
        .map(name => ({ name, expired: false, size_in_bytes: 100 }));
}
function scenario(overrides = {}) {
    let elapsed = 0;
    return { sha, currentRunId: 99, now: () => elapsed,
        sleep: async ms => { elapsed += ms; }, discoveryMs: 30, timeoutMs: 100, pollMs: 10,
        listRuns: async () => [run()], listArtifacts: async () => packages(), ...overrides };
}

test('reuse requires the Windows x64 package to be present and unexpired', () => {
    assert.equal(hasPackages(packages()), true);
    assert.equal(hasPackages(packages().slice(1)), false);
    for (const field of [{ expired: true }, { size_in_bytes: 0 }]) {
        const artifacts = packages();
        Object.assign(artifacts[0], field);
        assert.equal(hasPackages(artifacts), false);
    }
});

test('reuses a successful main build of the exact tagged SHA', async () => {
    assert.equal(await resolveBuild(scenario()), '10');
});

test('never reuses another SHA, branch, pull request, or the current run', async () => {
    for (const invalid of [
        { head_sha: 'b'.repeat(40) }, { head_branch: 'feature' },
        { event: 'pull_request' }, { id: 99 },
    ]) {
        assert.equal(await resolveBuild(scenario({ listRuns: async () => [run(invalid)],
            listArtifacts: async () => { assert.fail('Untrusted run must not be inspected.'); } })), '');
    }
});

test('waits for a main build that is still running', async () => {
    let polls = 0;
    assert.equal(await resolveBuild(scenario({ listRuns: async () => [run(++polls < 3
        ? { status: 'in_progress', conclusion: null } : {})] })), '10');
    assert.equal(polls, 3);
});

test('allows the simultaneous main push time to appear', async () => {
    let polls = 0;
    assert.equal(await resolveBuild(scenario({ listRuns: async () => ++polls < 3 ? [] : [run()] })), '10');
    assert.equal(polls, 3);
});

test('tag-only commits fall back after the discovery window', async () => {
    const input = scenario({ listRuns: async () => [] });
    assert.equal(await resolveBuild(input), '');
    assert.equal(input.now(), input.discoveryMs);
});

test('failed or canceled main builds require a fresh tested build', async () => {
    for (const conclusion of ['failure', 'cancelled', 'timed_out']) {
        const input = scenario({ listRuns: async () => [run({ conclusion })] });
        assert.equal(await resolveBuild(input), '');
        assert.equal(input.now(), 0);
    }
});

test('successful builds with missing or expired artifacts fall back', async () => {
    for (const artifacts of [[], packages().map(a => ({ ...a, expired: true }))]) {
        assert.equal(await resolveBuild(scenario({ listArtifacts: async () => artifacts })), '');
    }
});

test('an older complete successful build is usable when a newer run failed', async () => {
    assert.equal(await resolveBuild(scenario({ listRuns: async () => [
        run({ id: 11, conclusion: 'failure' }), run(),
    ] })), '10');
});

test('waiting timeout fails rather than starting a duplicate queued build', async () => {
    await assert.rejects(resolveBuild(scenario({ listRuns: async () => [run({
        status: 'queued', conclusion: null,
    })] })), /refusing to start a duplicate/);
});

test('API failures are propagated instead of silently triggering duplicate work', async () => {
    await assert.rejects(resolveBuild(scenario({ listRuns: async () => {
        throw new Error('API unavailable');
    } })), /API unavailable/);
});

test('GitHub adapter scopes queries and emits reusable run outputs', async () => {
    const outputs = {};
    const queries = [];
    const github = {
        rest: { actions: { listWorkflowRuns: 'runs', listWorkflowRunArtifacts: 'artifacts' } },
        paginate: async (endpoint, query) => {
            queries.push([endpoint, query]);
            return endpoint === 'runs' ? [run()] : packages();
        },
    };
    await resolveReleaseBuild({ github,
        context: { repo: { owner: 'owner', repo: 'fastpdf' }, sha, runId: 99 },
        core: { info: () => {}, setOutput: (key, value) => { outputs[key] = value; } },
    });
    assert.deepEqual(outputs, { 'run-id': '10', 'build-required': false });
    assert.equal(queries[0][1].head_sha, sha);
    assert.equal(queries[0][1].branch, 'main');
    assert.equal(queries[0][1].workflow_id, 'build-and-release.yml');
    assert.equal(queries[1][1].run_id, 10);
});
