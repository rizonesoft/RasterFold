// RasterFold campaign run guard for OpenCode (D00 T01 §10): the Stop hook and the heartbeat delivery.
//
// A turn ends as session.execution.succeeded, failed or interrupted (OpenCode V2 never emits session.idle,
// anomalyco/opencode#55358). On each turn end of a session in this checkout the plugin asks the shared guard,
// through scripts/opencode-campaign-hook.py, whether the run owning that session must continue, and queues the
// guard's reason into the session when it must. Every 30 seconds it polls scripts/opencode-heartbeat.py due and
// queues each due job's prompt into the job's session. A prompt is queued only into a session proven idle, at most
// one at a time per session, and never from a stale turn or after unload. Every failure queues nothing.
import { spawn } from "node:child_process"
import fs from "node:fs"
import path from "node:path"
import { fileURLToPath } from "node:url"

export const POLL_MS = 30_000
export const IN_FLIGHT_MS = 120_000
export const HOOK_TIMEOUT_MS = 70_000
export const DUE_TIMEOUT_MS = 30_000
const MAX_OUTPUT = 1_000_000
const END = {
  "session.execution.succeeded": false,
  "session.execution.failed": false,
  "session.execution.interrupted": true,
}

export function createCampaign({ root, runJSON, sessionGet, prompt, subscribe, now = Date.now,
                                 setInterval: startTimer = setInterval, clearInterval: stopTimer = clearInterval,
                                 log = () => {} }) {
  const sessions = new Map()
  const scope = new Map()
  let disposed = false
  let timer
  let ticking = false
  let controller

  const get = (id) => {
    if (!sessions.has(id)) sessions.set(id, { state: undefined, epoch: 0, pending: false, inFlightUntil: 0 })
    return sessions.get(id)
  }

  async function inScope(id) {
    if (scope.has(id)) return scope.get(id)
    try {
      const dir = (await sessionGet(id))?.location?.directory
      if (typeof dir !== "string") return false
      const resolved = path.resolve(dir)
      const inside = resolved === root || resolved.startsWith(root + path.sep)
      scope.set(id, inside)
      return inside
    } catch (error) {
      log(`scope check failed for ${id}: ${error}`)
      return false
    }
  }

  async function isIdle(id) {
    const s = get(id)
    if (s.state === "idle") return true
    if (s.state === "busy") return false
    // Not seen since the plugin (re)subscribed: idle only when the server's last idle time covers its last update.
    try {
      const time = (await sessionGet(id))?.time
      return !!time && typeof time.idle === "number" && typeof time.updated === "number" && time.idle >= time.updated
    } catch (error) {
      log(`idle check failed for ${id}: ${error}`)
      return false
    }
  }

  async function submit(id, text, epoch) {
    const s = get(id)
    if (disposed || s.pending || s.inFlightUntil > now()) return false
    if (epoch !== undefined && s.epoch !== epoch) return false
    s.pending = true
    try {
      if (!(await isIdle(id))) return false
      if (disposed || s.inFlightUntil > now() || (epoch !== undefined && s.epoch !== epoch)) return false
      s.inFlightUntil = now() + IN_FLIGHT_MS
      await prompt(id, text)
      return true
    } catch (error) {
      s.inFlightUntil = 0
      log(`prompt failed for ${id}: ${error}`)
      return false
    } finally {
      s.pending = false
    }
  }

  async function onEvent(event) {
    if (disposed || !event || typeof event.type !== "string") return
    const id = event.data?.sessionID
    if (typeof id !== "string" || !id) return
    if (event.type === "session.execution.started") {
      const s = get(id)
      s.epoch += 1
      s.state = "busy"
      s.inFlightUntil = 0
      return
    }
    if (!(event.type in END)) return
    const s = get(id)
    s.epoch += 1
    s.state = "idle"
    const epoch = s.epoch
    if (!(await inScope(id))) return
    const decision = await runJSON("scripts/opencode-campaign-hook.py", [],
      { session_id: id, cwd: root, interrupted: END[event.type] }, HOOK_TIMEOUT_MS)
    if (decision?.decision === "block" && typeof decision.reason === "string" && decision.reason) {
      await submit(id, decision.reason, epoch)
    }
  }

  async function tick() {
    if (disposed || ticking) return
    ticking = true
    try {
      const due = await runJSON("scripts/opencode-heartbeat.py", ["due"], undefined, DUE_TIMEOUT_MS)
      if (!Array.isArray(due)) return
      for (const job of due) {
        if (disposed) return
        if (typeof job?.session !== "string" || typeof job?.prompt !== "string" || !job.prompt) continue
        await submit(job.session, job.prompt)
      }
    } catch (error) {
      log(`heartbeat poll failed: ${error}`)
    } finally {
      ticking = false
    }
  }

  async function listen() {
    while (!disposed) {
      controller = new AbortController()
      try {
        for await (const event of subscribe(controller.signal)) {
          if (disposed) return
          void onEvent(event).catch((error) => log(`turn-end handling failed: ${error}`))
        }
      } catch (error) {
        if (!disposed) log(`event stream failed: ${error}`)
      }
      if (disposed) return
      // Events may have been missed: nothing is known about any session until it reports again.
      for (const s of sessions.values()) s.state = undefined
      await new Promise((resolve) => setTimeout(resolve, 1000))
    }
  }

  return {
    start() {
      timer = startTimer(() => void tick(), POLL_MS)
      void listen()
    },
    dispose() {
      disposed = true
      if (timer !== undefined) stopTimer(timer)
      controller?.abort()
    },
    onEvent,
    tick,
    submit,
    get disposed() {
      return disposed
    },
  }
}

export function runPython(root, script, args, input, timeoutMs) {
  return new Promise((resolve) => {
    let out = ""
    let done = false
    const finish = (value) => {
      if (!done) {
        done = true
        clearTimeout(killer)
        resolve(value)
      }
    }
    let child
    try {
      child = spawn(process.platform === "win32" ? "python" : "python3", [path.join(root, script), ...args],
        { cwd: root, stdio: ["pipe", "pipe", "ignore"], windowsHide: true })
    } catch {
      resolve(null)
      return
    }
    const killer = setTimeout(() => {
      child.kill()
      finish(null)
    }, timeoutMs)
    child.on("error", () => finish(null))
    child.stdout.on("data", (chunk) => {
      out += chunk
      if (out.length > MAX_OUTPUT) {
        child.kill()
        finish(null)
      }
    })
    child.on("close", () => {
      try {
        finish(JSON.parse(out))
      } catch {
        finish(null)
      }
    })
    child.stdin.on("error", () => {})
    child.stdin.end(input === undefined ? "" : JSON.stringify(input))
  })
}

export function checkoutRoot(moduleUrl = import.meta.url) {
  const root = path.resolve(path.dirname(fileURLToPath(moduleUrl)), "..", "..", "..")
  const needed = ["scripts/campaign_guard.py", "scripts/opencode-campaign-hook.py", "scripts/opencode-heartbeat.py"]
  return needed.every((file) => fs.existsSync(path.join(root, file))) ? root : undefined
}

export default {
  id: "rasterfold-campaign",
  setup(ctx) {
    const root = checkoutRoot()
    if (!root) return
    const campaign = createCampaign({
      root,
      runJSON: (script, args, input, timeoutMs) => runPython(root, script, args, input, timeoutMs),
      sessionGet: (sessionID) => ctx.session.get({ sessionID }),
      prompt: (sessionID, text) => ctx.session.prompt({ sessionID, text }),
      subscribe: (signal) => ctx.event.subscribe({ signal }),
      log: (message) => console.error(`rasterfold-campaign: ${message}`),
    })
    campaign.start()
    return () => campaign.dispose()
  },
}
