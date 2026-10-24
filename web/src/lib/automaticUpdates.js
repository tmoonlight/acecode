import { desktopUpdateRestartAvailable, updateJobIsActive } from './updateJob.js';

export const MIN_UPDATE_CHECK_DELAY_MS = 10 * 60 * 1000;
export const MAX_UPDATE_CHECK_DELAY_MS = 2 * 60 * 60 * 1000;

export function randomUpdateCheckDelay(random = Math.random) {
  const value = Number(random());
  const fraction = Number.isFinite(value) ? Math.max(0, Math.min(1, value)) : 0;
  return MIN_UPDATE_CHECK_DELAY_MS
    + Math.floor(fraction * (MAX_UPDATE_CHECK_DELAY_MS - MIN_UPDATE_CHECK_DELAY_MS));
}

export function automaticUpdateFailed(status, job) {
  return job?.target_version === status?.latest_version && Number(job?.automatic_failures) >= 3;
}

// One owner for checks, starts and polling. Transport failures never invent a
// job outcome; the daemon owns installation state and consecutive failures.
export function createUpdateController({
  api, notify = () => {}, random = Math.random,
  isCurrentConnection = () => true,
  setTimer = setTimeout, clearTimer = clearTimeout,
}) {
  let state = { status: null, job: null, checking: false, starting: false, dialogOpen: false };
  const listeners = new Set();
  const notified = new Set();
  let active = false;
  let generation = 0;
  let pollGeneration = 0;
  let checkTimer;
  let pollTimer;
  let checking;
  let starting;
  let refreshRequested = false;
  let interactiveJobId = '';
  const publish = (patch) => {
    state = { ...state, ...patch };
    listeners.forEach((listener) => listener());
  };
  const live = (id) => active && generation === id && isCurrentConnection();
  const ready = () => state.job?.state === 'succeeded' && state.job?.restart_required;

  function acceptJob(job, { interactive = false, terminal = false } = {}) {
    if (!job?.job_id) return;
    if (interactive) interactiveJobId = job.job_id;
    publish({ job, ...(interactive ? { dialogOpen: true } : {}) });
    if (!terminal || !['succeeded', 'failed', 'cancelled'].includes(job.state) || notified.has(job.job_id)) return;
    notified.add(job.job_id);
    if (job.automatic) {
      if (job.state === 'succeeded') notify({ kind: 'ok', text: '更新已完成，重启后生效' });
      return;
    }
    if (interactiveJobId !== job.job_id) return;
    publish({ dialogOpen: true });
    if (job.state === 'succeeded') notify({
      kind: 'ok',
      text: desktopUpdateRestartAvailable()
        ? '升级安装完成，可立即重启 ACECode'
        : '升级安装完成，请完全退出并重新启动 ACECode',
    });
    else if (job.state === 'failed') notify({ kind: 'err', text: '升级失败:' + (job.error || '未知错误') });
    else if (job.state === 'cancelled') notify({ kind: 'info', text: '升级已取消，当前安装未修改' });
  }

  function poll(jobId) {
    clearTimer(pollTimer);
    const pollId = ++pollGeneration;
    const id = generation;
    let failures = 0;
    const valid = () => live(id) && pollGeneration === pollId;
    const tick = async () => {
      try {
        const job = await api.getUpdateJob(jobId);
        if (!valid()) return;
        failures = 0;
        acceptJob(job, { terminal: true });
        if (updateJobIsActive(job)) pollTimer = setTimer(tick, 350);
      } catch {
        if (!valid()) return;
        failures = Math.min(failures + 1, 7);
        pollTimer = setTimer(tick, Math.min(60000, 600 * 2 ** failures));
      }
    };
    void tick();
  }

  async function restore(id, interactive = false) {
    try {
      const job = await api.getLatestUpdateJob();
      if (!live(id)) return;
      acceptJob(job, { interactive: interactive && !job.automatic && updateJobIsActive(job) });
      if (updateJobIsActive(job)) poll(job.job_id);
    } catch { /* No job or an offline daemon: keep the last known state. */ }
  }

  function startUpdate({ automatic = false } = {}) {
    if (!live(generation) || starting || updateJobIsActive(state.job) || ready() || !state.status?.update_available) {
      return starting || Promise.resolve();
    }
    const id = generation;
    const work = Promise.resolve().then(async () => {
      try {
        if (automatic) {
          const config = await api.getUpgradeConfig();
          if (!live(id) || config?.auto_update !== true) return;
        }
        if (!live(id)) return;
        publish({ starting: true });
        const job = await api.startUpdate({ automatic });
        if (!live(id)) return;
        acceptJob(job, { interactive: !automatic, terminal: true });
        if (updateJobIsActive(job)) poll(job.job_id);
      } catch (error) {
        if (!live(id)) return;
        if (error?.body?.job) {
          const job = error.body.job;
          acceptJob(job, { interactive: !automatic, terminal: true });
          if (updateJobIsActive(job)) poll(job.job_id);
        } else if (!automatic) {
          notify({ kind: 'err', text: '启动升级失败:' + (error?.message || '未知错误') });
        }
      } finally {
        if (live(id)) publish({ starting: false });
        if (starting === work) starting = null;
      }
    });
    starting = work;
    return work;
  }

  function schedule() {
    clearTimer(checkTimer);
    if (active) checkTimer = setTimer(() => { void check(); }, randomUpdateCheckDelay(random));
  }

  function check({ manual = false, recoverInteractive = false } = {}) {
    if (!live(generation)) return Promise.resolve();
    if (manual) publish({ checking: true });
    if (checking) {
      // A manual request can join a background check without another request.
      const id = generation;
      return checking.then((result) => {
        if (!live(id) || !manual) return result;
        publish({ checking: false });
        if (result?.error) notify({ kind: 'err', text: '检查更新失败:' + (result.error?.message || '未知错误') });
        else if (result?.status) publish({ dialogOpen: true });
        return result;
      });
    }
    const id = generation;
    const work = Promise.resolve().then(async () => {
      try {
        await restore(id, recoverInteractive);
        if (!live(id)) return;
        const status = await api.getUpdateStatus();
        if (!live(id)) return;
        if (!['available', 'up_to_date', 'store_managed'].includes(status?.status)) {
          throw new Error(status?.error || '更新服务返回无效响应');
        }
        const keepJob = updateJobIsActive(state.job) || ready();
        const stale = state.job?.target_version && state.job.target_version !== status.latest_version;
        publish({ status, ...(!keepJob && (stale || status.status !== 'available') ? { job: null } : {}) });
        if (manual) publish({ dialogOpen: true });
        else await startUpdate({ automatic: true });
        return { status };
      } catch (error) {
        if (live(id) && manual) notify({ kind: 'err', text: '检查更新失败:' + (error?.message || '未知错误') });
        return { error };
      } finally {
        if (checking === work) checking = null;
        if (live(id)) {
          publish({ checking: false });
          if (refreshRequested) { refreshRequested = false; void check(); }
          else schedule();
        }
      }
    });
    checking = work;
    return work;
  }

  return {
    getSnapshot: () => state,
    subscribe(listener) { listeners.add(listener); return () => listeners.delete(listener); },
    check,
    startUpdate,
    refreshConfiguration() {
      if (!live(generation)) return;
      if (checking) refreshRequested = true;
      else void check();
    },
    setDialogOpen(value) { publish({ dialogOpen: value }); },
    setJob(job) {
      if (!live(generation)) return;
      clearTimer(pollTimer);
      pollGeneration += 1;
      acceptJob(job, { terminal: true });
      if (updateJobIsActive(job)) poll(job.job_id);
    },
    openDialog() {
      const stale = !updateJobIsActive(state.job) && !ready() && state.job?.target_version
        && state.status?.latest_version && state.job.target_version !== state.status.latest_version;
      publish({ dialogOpen: true, ...(stale ? { job: null } : {}) });
    },
    async start() {
      if (active) return;
      active = true;
      generation += 1;
      await check({ recoverInteractive: true });
    },
    stop() {
      active = false;
      generation += 1;
      pollGeneration += 1;
      clearTimer(checkTimer);
      clearTimer(pollTimer);
      checking = null;
      starting = null;
      refreshRequested = false;
      publish({ checking: false, starting: false });
    },
  };
}
