const packageNames = ['fastpdf-windows-x64'];

function hasPackages(artifacts) {
    return packageNames.every(name => artifacts.some(artifact =>
        artifact.name === name && !artifact.expired && artifact.size_in_bytes > 0));
}

// Inject time and API calls so waiting, failures, and artifact expiry can be tested without GitHub.
async function resolveBuild({ listRuns, listArtifacts, sha, currentRunId,
    now = Date.now, sleep = ms => new Promise(resolve => setTimeout(resolve, ms)),
    log = () => {}, discoveryMs = 120000, timeoutMs = 2700000, pollMs = 15000 }) {
    const started = now();
    for (;;) {
        const runs = (await listRuns()).filter(run =>
            run.head_sha === sha && run.head_branch === 'main' && run.id !== currentRunId &&
            ['push', 'workflow_dispatch'].includes(run.event));
        for (const run of runs.filter(run => run.status === 'completed' && run.conclusion === 'success')) {
            if (hasPackages(await listArtifacts(run.id))) return String(run.id);
        }
        const pending = runs.find(run => run.status !== 'completed');
        if (!pending && (runs.length > 0 || now() - started >= discoveryMs)) return '';
        if (now() - started >= timeoutMs) {
            throw new Error('Timed out waiting for the existing main build; refusing to start a duplicate. Retry the release after it finishes.');
        }
        log(pending ? `Waiting for main build ${pending.id} of ${sha}.` : `Waiting for the main push of ${sha} to appear.`);
        await sleep(pollMs);
    }
}

async function resolveReleaseBuild({ github, context, core }) {
    const repo = context.repo;
    const runId = await resolveBuild({
        sha: context.sha, currentRunId: context.runId, log: core.info,
        listRuns: () => github.paginate(github.rest.actions.listWorkflowRuns, {
            ...repo, workflow_id: 'build-and-release.yml', head_sha: context.sha,
            branch: 'main', per_page: 100,
        }),
        listArtifacts: run_id => github.paginate(github.rest.actions.listWorkflowRunArtifacts, {
            ...repo, run_id, per_page: 100,
        }),
    });
    core.setOutput('run-id', runId);
    core.setOutput('build-required', runId === '');
    core.info(runId ? `Reusing the Windows x64 package from run ${runId}.` : 'No usable main build; building the tagged commit.');
}

module.exports = { hasPackages, resolveBuild, resolveReleaseBuild };
