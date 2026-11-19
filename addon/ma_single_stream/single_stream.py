"""Keep a single YouTube Music stream running in Music Assistant.

The account allows one stream at a time. Every second this reads the Music
Assistant queues. When two queues play YouTube Music at once, the one that
started last wins and the players of the others are grouped to it, so they
all play the same stream. One exception: when the Brimbelle speaker started
because of a click on its knob (resume, or the Home Assistant automation that
answers a click while idle), the Brimbelle joins the stream that was already
playing instead.

Players grouped here are ungrouped again once the stream they joined stops.

Music Assistant refuses to group a player that still plays and has followers
of its own, and only logs a warning. So the group is checked on the next
polls and asked again for a few seconds, once the player has stopped.

A click on the knob while the Brimbelle follows another player takes only the
Brimbelle out of the group. The click itself pauses or resumes the whole
group (that is what the speaker sends), so the queue of the group is put back
in the state it had before the click.

Settings come from /data/options.json (the add-on options). The knob click is
read from the Home Assistant event entity of the speaker.
"""
import json
import os
import time
import urllib.request

PROVIDER = "ytmusic"


def stream_provider(queue):
    """Domain of the provider that serves the current item of a queue."""
    item = queue.get("current_item") or {}
    provider = (item.get("streamdetails") or {}).get("provider")
    if not provider:
        # No stream details yet: trust the track only when it has one source.
        domains = {m.get("provider_domain") for m in (item.get("media_item") or {}).get("provider_mappings") or []}
        return domains.pop() if len(domains) == 1 else None
    return provider.split("--")[0]


def plays_provider(queue):
    return queue.get("state") == "playing" and stream_provider(queue) == PROVIDER


class Arbiter:
    """Pure logic: evaluate() turns the current state into a list of actions.

    Actions: ("stop", queue_id), ("group", leader_id, [player_ids]),
    ("ungroup", player_id), ("play", queue_id), ("pause", queue_id).
    """

    def __init__(self, brimbelle_id, button_window=15.0, idle_grace=5.0, cooldown=5.0, restore_window=3.0, regroup_window=10.0):
        self.brimbelle_id = brimbelle_id
        self.button_window = button_window
        self.idle_grace = idle_grace
        self.cooldown = cooldown
        self.restore_window = restore_window
        self.regroup_window = regroup_window
        self.button_at = None   # time of the last knob click
        self.clicked = False    # a click not looked at yet
        self.history = []       # (time, {queue_id: state}) of the last seconds
        self.restore = None     # (queue_id, state to put back, until)
        self.since = {}         # queue_id -> time it started a YouTube Music stream
        self.handled = {}       # queue_id -> time we last moved its players
        self.joined = {}        # player_id -> (leader_id, time) for groups made here
        self.idle_since = {}    # leader_id -> time its queue went idle

    def button(self, now):
        self.button_at = now
        self.clicked = True

    def leave_group(self, queues, players, now):
        """Click while the Brimbelle follows another player: only it leaves."""
        actions = []
        if self.clicked:
            self.clicked = False
            leader = (players.get(self.brimbelle_id) or {}).get("synced_to")
            if leader:
                actions.append(("ungroup", self.brimbelle_id))
                self.joined.pop(self.brimbelle_id, None)
                # The state from a few seconds ago: the click may already show.
                before = self.history[0][1].get(leader) if self.history else None
                if before in ("playing", "paused"):
                    self.restore = (leader, before, now + self.restore_window)
        if self.restore:
            leader, before, until = self.restore
            state = (queues.get(leader) or {}).get("state")
            if now > until:
                self.restore = None
            elif state in ("playing", "paused") and state != before:
                actions.append(("play" if before == "playing" else "pause", leader))
        self.history.append((now, {qid: q.get("state") for qid, q in queues.items()}))
        while len(self.history) > 1 and now - self.history[0][0] > self.restore_window:
            self.history.pop(0)
        return actions

    def evaluate(self, queues, players, now):
        """queues and players: dicts by id, as Music Assistant returns them."""
        actions = self.leave_group(queues, players, now)
        active = [qid for qid, q in queues.items() if plays_provider(q)]
        for qid in list(self.since):
            if qid not in active:
                del self.since[qid]
        for qid in active:
            self.since.setdefault(qid, now)

        if len(active) > 1:
            order = sorted(active, key=lambda qid: self.since[qid])
            winner = order[-1]
            clicked = self.button_at is not None and now - self.button_at <= self.button_window
            if winner == self.brimbelle_id and clicked:
                winner = order[-2]
            for loser in order:
                if loser == winner or now - self.handled.get(loser, -self.cooldown) < self.cooldown:
                    continue
                self.handled[loser] = now
                actions.append(("stop", loser))
                members = (players.get(loser) or {}).get("group_members") or []
                movers = [loser] + [m for m in members if m != loser]
                movers = [m for m in movers if winner in ((players.get(m) or {}).get("can_group_with") or [])]
                if movers:
                    actions.append(("group", winner, movers))
                    for m in movers:
                        self.joined[m] = (winner, now)

        for member, (leader, at) in list(self.joined.items()):
            if (queues.get(leader) or {}).get("state", "idle") != "idle":
                self.idle_since.pop(leader, None)
                # The group did not hold (refused by Music Assistant): ask again.
                if ((players.get(member) or {}).get("synced_to") != leader
                        and (queues.get(member) or {}).get("state") != "playing"
                        and 0 < now - at <= self.regroup_window):
                    actions.append(("group", leader, [member]))
                continue
            idle_at = self.idle_since.setdefault(leader, now)
            if now - idle_at >= self.idle_grace and now - at >= self.idle_grace:
                if (players.get(member) or {}).get("synced_to") == leader:
                    actions.append(("ungroup", member))
                del self.joined[member]
        return actions


def call_json(url, token, body=None):
    req = urllib.request.Request(url, data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read() or b"null")


def main():
    with open(os.environ.get("OPTIONS", "/data/options.json"), encoding="utf-8") as f:
        opt = json.load(f)
    ma_url = opt["ma_url"].rstrip("/") + "/api"
    ha_url = os.environ.get("HA_URL", "http://supervisor/core/api") + "/states/" + opt["button_entity"]
    ha_token = os.environ.get("SUPERVISOR_TOKEN", "")
    dry_run = opt.get("dry_run", False)
    arbiter = Arbiter(opt["brimbelle_player_id"], button_window=opt.get("button_window", 15))

    def ma(command, **args):
        return call_json(ma_url, opt["ma_token"], {"command": command, "args": args})

    def names(ids, players):
        return ", ".join((players.get(i) or {}).get("name") or i for i in ids)

    print("single stream: started%s" % (" (dry run: nothing is changed)" if dry_run else ""), flush=True)
    last_click = None
    failing = False
    while True:
        try:
            click = call_json(ha_url, ha_token)
            if click["state"] != last_click:
                kind = click["attributes"].get("event_type")
                if last_click is not None and kind in ("play_request", "toggle"):
                    print("knob click (%s)" % kind, flush=True)
                    arbiter.button(time.monotonic())
                last_click = click["state"]
            players = {p["player_id"]: p for p in ma("players/all")}
            queues = {q["queue_id"]: q for q in ma("player_queues/all")}
            for action in arbiter.evaluate(queues, players, time.monotonic()):
                if action[0] == "stop":
                    print("stop the queue of %s" % names([action[1]], players), flush=True)
                    dry_run or ma("player_queues/stop", queue_id=action[1])
                elif action[0] == "group":
                    print("group %s to %s" % (names(action[2], players), names([action[1]], players)), flush=True)
                    dry_run or ma("players/cmd/group_many", target_player=action[1], child_player_ids=action[2])
                elif action[0] == "ungroup":
                    print("ungroup %s" % names([action[1]], players), flush=True)
                    dry_run or ma("players/cmd/ungroup", player_id=action[1])
                else:
                    print("%s the queue of %s again" % (action[0], names([action[1]], players)), flush=True)
                    dry_run or ma("player_queues/" + action[0], queue_id=action[1])
            if failing:
                print("back to normal", flush=True)
                failing = False
        except Exception as e:  # network errors, Music Assistant restarting: keep going
            if not failing:
                print("error, retrying every second: %r" % e, flush=True)
                failing = True
        time.sleep(1)


if __name__ == "__main__":
    main()
