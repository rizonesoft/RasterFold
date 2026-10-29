// Drives .opencode/plugins/rasterfold-campaign/index.js against a fake OpenCode context, adapter and clock.
// Prints "ok <case>" per case and exits non-zero on the first failure; scripts/tests/test_opencode_plugin.py runs it.
import assert from "node:assert/strict"
import path from "node:path"
import { pathToFileURL } from "node:url"

const plugin = await import(pathToFileURL(path.resolve(process.argv[2])).href)
const { createCampaign, IN_FLIGHT_MS } = plugin
const ROOT = path.resolve("/work/RasterFold")
const tick = () => new Promise((resolve) => setImmediate(resolve))

function fixture({ hook = () => ({ decision: "block", reason: "keep shipping" }), due = () => [],
                   info = (id) => ({ location: { directory: ROOT }, time: { updated: 1, idle: 2 } }) } = {}) {
  const prompts = []
  const timers = new Set()
  let clock = 1_000_000
  const deferred = []
  const streams = []
  const env = {
    prompts,
    timers,
    deferred,
    streams,
    advance(ms) { clock += ms },
    campaign: createCampaign({
      root: ROOT,
      now: () => clock,
      runJSON: async (script, args, input) => {
        if (script.endsWith("opencode-campaign-hook.py")) {
          const result = hook(input)
          return result instanceof Promise ? result : result
        }
        return due()
      },
      sessionGet: async (id) => {
        const value = info(id)
        if (value instanceof Error) throw value
        return value
      },
      prompt: async (id, text) => { prompts.push([id, text]) },
      subscribe: (signal) => {
        const queue = []
        let wake
        streams.push({ push(event) { queue.push(event); wake?.() }, signal })
        return (async function* () {
          while (!signal.aborted) {
            if (queue.length) { yield queue.shift(); continue }
            await new Promise((resolve) => { wake = resolve; signal.addEventListener("abort", resolve, { once: true }) })
          }
        })()
      },
      setInterval: (fn, ms) => { const t = { fn, ms }; timers.add(t); return t },
      clearInterval: (t) => { timers.delete(t) },
    }),
  }
  return env
}

const ev = (type, id = "ses_a") => ({ type, data: { sessionID: id } })
const cases = {}

cases["a turn end the guard blocks queues its reason"] = async () => {
  const f = fixture()
  await f.campaign.onEvent(ev("session.execution.started"))
  await f.campaign.onEvent(ev("session.execution.succeeded"))
  assert.deepEqual(f.prompts, [["ses_a", "keep shipping"]])
}

cases["an interrupted turn is passed on as interrupted and queues nothing"] = async () => {
  const seen = []
  const f = fixture({ hook: (input) => { seen.push(input.interrupted); return input.interrupted ? {} : { decision: "block", reason: "x" } } })
  await f.campaign.onEvent(ev("session.execution.started"))
  await f.campaign.onEvent(ev("session.execution.interrupted"))
  assert.deepEqual(seen, [true])
  assert.deepEqual(f.prompts, [])
}

cases["no decision, a malformed decision or a failed adapter queues nothing"] = async () => {
  for (const result of [{}, null, { decision: "block" }, { decision: "block", reason: "" }, { decision: "allow", reason: "x" }]) {
    const f = fixture({ hook: () => result })
    await f.campaign.onEvent(ev("session.execution.succeeded"))
    assert.deepEqual(f.prompts, [], JSON.stringify(result))
  }
}

cases["a delayed block is dropped when another execution started or was interrupted meanwhile"] = async () => {
  for (const later of [["session.execution.started"], ["session.execution.started", "session.execution.interrupted"]]) {
    let release
    const f = fixture({ hook: (input) => input.interrupted ? {} : new Promise((resolve) => { release = () => resolve({ decision: "block", reason: "stale" }) }) })
    const first = f.campaign.onEvent(ev("session.execution.succeeded"))
    await tick()
    for (const type of later) await f.campaign.onEvent(ev(type))
    release()
    await first
    assert.deepEqual(f.prompts, [], later.join(","))
  }
}

cases["a heartbeat and a block for the same idle session queue one prompt"] = async () => {
  let release
  const f = fixture({ hook: () => new Promise((resolve) => { release = () => resolve({ decision: "block", reason: "block" }) }),
                      due: () => [{ id: "ohb-1", session: "ses_a", prompt: "heartbeat" }] })
  await f.campaign.onEvent(ev("session.execution.started"))
  const end = f.campaign.onEvent(ev("session.execution.succeeded"))
  const beat = f.campaign.tick()
  await tick()
  release()
  await Promise.all([end, beat])
  assert.equal(f.prompts.length, 1)
  await f.campaign.tick()
  assert.equal(f.prompts.length, 1, "a second prompt while the first is in flight")
  f.advance(IN_FLIGHT_MS + 1)
  await f.campaign.tick()
  assert.equal(f.prompts.length, 2, "the in-flight mark expires")
}

cases["a heartbeat fires for an idle session whoever owns the guard and skips a busy one"] = async () => {
  // The guard names another session (or none), so the hook never blocks; the heartbeat still delivers.
  const f = fixture({ hook: () => ({}), due: () => [{ id: "ohb-1", session: "ses_a", prompt: "heartbeat" }] })
  await f.campaign.onEvent(ev("session.execution.started"))
  await f.campaign.tick()
  assert.deepEqual(f.prompts, [], "busy session")
  await f.campaign.onEvent(ev("session.execution.interrupted"))
  await f.campaign.tick()
  assert.deepEqual(f.prompts, [["ses_a", "heartbeat"]])
}

cases["a session unseen since load is idle only when its idle time covers its last update"] = async () => {
  for (const [time, fires] of [[{ updated: 5, idle: 6 }, true], [{ updated: 7, idle: 6 }, false], [{ updated: 7 }, false], [undefined, false]]) {
    const f = fixture({ due: () => [{ id: "ohb-1", session: "ses_b", prompt: "heartbeat" }],
                        info: () => ({ location: { directory: ROOT }, time }) })
    await f.campaign.tick()
    assert.equal(f.prompts.length, fires ? 1 : 0, JSON.stringify(time))
  }
  const f = fixture({ due: () => [{ id: "ohb-1", session: "ses_b", prompt: "heartbeat" }], info: () => new Error("down") })
  await f.campaign.tick()
  assert.deepEqual(f.prompts, [], "a failed session read")
}

cases["a plugin loaded mid-execution queues nothing until the execution ends"] = async () => {
  const f = fixture({ due: () => [{ id: "ohb-1", session: "ses_a", prompt: "heartbeat" }],
                      info: () => ({ location: { directory: ROOT }, time: { updated: 9, idle: 4 } }) })
  await f.campaign.tick()
  assert.deepEqual(f.prompts, [])
  await f.campaign.onEvent(ev("session.execution.succeeded"))
  assert.deepEqual(f.prompts, [["ses_a", "keep shipping"]])
}

cases["a session outside the checkout is never asked about"] = async () => {
  let asked = 0
  const f = fixture({ hook: () => { asked += 1; return { decision: "block", reason: "x" } },
                      due: () => [{ id: "ohb-1", session: "ses_a", prompt: "heartbeat" }],
                      info: () => ({ location: { directory: "/elsewhere/project" }, time: { updated: 1, idle: 2 } }) })
  await f.campaign.onEvent(ev("session.execution.succeeded"))
  await f.campaign.tick()
  assert.equal(asked, 0)
  assert.deepEqual(f.prompts, [])
}

cases["malformed events and due entries are ignored"] = async () => {
  const f = fixture({ due: () => [null, {}, { session: "ses_a" }, { session: 5, prompt: "x" }, { session: "ses_a", prompt: "" }] })
  for (const event of [null, {}, { type: 5 }, { type: "session.execution.succeeded" }, { type: "session.execution.succeeded", data: { sessionID: "" } }]) {
    await f.campaign.onEvent(event)
  }
  await f.campaign.tick()
  assert.deepEqual(f.prompts, [])
}

cases["after cleanup nothing is queued and a reloaded instance is the only poller"] = async () => {
  let release
  const f = fixture({ hook: () => new Promise((resolve) => { release = () => resolve({ decision: "block", reason: "late" }) }),
                      due: () => [{ id: "ohb-1", session: "ses_a", prompt: "heartbeat" }] })
  f.campaign.start()
  assert.equal(f.timers.size, 1)
  await tick()
  assert.equal(f.streams.length, 1)
  const end = f.campaign.onEvent(ev("session.execution.succeeded"))
  await tick()
  f.campaign.dispose()
  release()
  await end
  await f.campaign.tick()
  f.streams[0].push(ev("session.execution.succeeded"))
  await tick()
  assert.deepEqual(f.prompts, [])
  assert.equal(f.timers.size, 0, "the poll timer was cleared")
  assert.ok(f.streams[0].signal.aborted, "the event subscription was aborted")
  const g = fixture({ due: () => [{ id: "ohb-1", session: "ses_a", prompt: "heartbeat" }] })
  g.campaign.start()
  assert.equal(f.timers.size + g.timers.size, 1)
  g.campaign.dispose()
}

let failed = 0
for (const [name, run] of Object.entries(cases)) {
  try {
    await run()
    console.log(`ok ${name}`)
  } catch (error) {
    failed += 1
    console.log(`FAIL ${name}: ${error.message}`)
  }
}
process.exit(failed ? 1 : 0)
