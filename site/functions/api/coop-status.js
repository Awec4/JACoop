// Cloudflare Pages Function: /api/coop-status
//
// The coop mod (AdvancedJK-Coop, sv_status.cpp) POSTs a JSON snapshot of the
// host every ~20s; the coop page GETs it and renders the live card.
//
// Setup (Cloudflare dashboard → Pages project → Settings):
//   1. Workers & Pages → KV → create a namespace, bind it to this project as
//      variable name COOP_STATUS (Pages project → Settings → Bindings → KV).
//   2. Settings → Environment variables → add COOP_STATUS_TOKEN (a long random
//      string). Put the same string in the game's cvar sv_statusToken.

const KEY = "status";

export async function onRequestPost({ request, env }) {
	let body;
	try {
		body = await request.json();
	} catch {
		return new Response("bad request", { status: 400 });
	}

	if (!env.COOP_STATUS_TOKEN || body.token !== env.COOP_STATUS_TOKEN) {
		return new Response("forbidden", { status: 403 });
	}

	const status = {
		hostname: String(body.hostname || "").slice(0, 64),
		map: String(body.map || "").slice(0, 64),
		players: Math.max(0, Math.min(64, parseInt(body.players, 10) || 0)),
		maxplayers: Math.max(0, Math.min(64, parseInt(body.maxplayers, 10) || 0)),
		lobby: !!body.lobby,
		join: String(body.join || "").slice(0, 128),
		protocol: parseInt(body.protocol, 10) || 0,
		updatedAt: Date.now(),
	};

	await env.COOP_STATUS.put(KEY, JSON.stringify(status));
	return new Response(null, { status: 204 });
}

export async function onRequestGet({ env }) {
	const raw = await env.COOP_STATUS.get(KEY);
	return new Response(raw || JSON.stringify({ offline: true }), {
		headers: {
			"content-type": "application/json",
			"cache-control": "no-store",
		},
	});
}
