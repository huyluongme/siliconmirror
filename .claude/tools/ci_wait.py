"""Wait for the GitHub Actions runs of a commit; print their jobs' results.

Unauthenticated, the API allows 60 requests an hour: poll once a minute,
and on a 403 rate limit sleep until the limit resets (from rate_limit,
which does not count), then go on.
"""
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request

API = "https://api.github.com/repos/huyluongme/siliconmirror/actions"
# head_sha matches the full SHA only: expand a short one.
SHA = subprocess.run(["git", "rev-parse", sys.argv[1]], capture_output=True,
                     text=True, check=True).stdout.strip()


def raw(url):
	req = urllib.request.Request(url, headers={"User-Agent": "ci-wait",
	                                           "Accept": "application/vnd.github+json"})
	with urllib.request.urlopen(req, timeout=30) as r:
		return json.load(r)


def get(url):
	while True:
		try:
			return raw(url)
		except urllib.error.HTTPError as e:
			if e.code != 403:
				raise
			reset = raw("https://api.github.com/rate_limit")["resources"]["core"]["reset"]
			wait = max(reset - time.time(), 0) + 5
			print(f"rate limited: sleeping {int(wait)} s", flush=True)
			time.sleep(wait)


while True:
	runs = get(f"{API}/runs?head_sha={SHA}")["workflow_runs"]
	waiting = [(r["name"], r["status"]) for r in runs if r["status"] != "completed"]
	if runs and not waiting:
		break
	print("waiting:", waiting or "no run yet", flush=True)
	time.sleep(60)
for r in runs:
	print(r["name"], r["conclusion"])
	for j in get(r["jobs_url"])["jobs"]:
		print("  ", j["name"], j["conclusion"])
