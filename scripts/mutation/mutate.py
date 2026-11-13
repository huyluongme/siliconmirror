#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc
"""Mutation testing for siliconmirror, the full run.

A mutant is a copy of the code with one small change, such as < made <=,
a constant made one larger, or a statement deleted. The tests should
fail on every mutant (they "kill" it); a mutant that passes every test
"survives", and shows either a gap in the tests or a change that makes
no difference (an equivalent mutant).

Every mutant is built with every target, with AddressSanitizer and
UBSan, and checked against the whole test suite. Mutants run in copies
of the repository under ~/.cache/siliconmirror-mutation, so the working
tree is never touched, several at once.

Usage:
  mutate.py CONFIG LOG            run every mutant
  mutate.py CONFIG LOG SURVIVORS  run again the survivors in an earlier LOG
  mutate.py CONFIG --count        count the mutants, run nothing

Each result goes to LOG.results as soon as it is known; the same command
goes on from there after a stop. LOG gets the survivors and the totals
at the end. Use a new LOG for a new run.

CONFIG is a Python file that sets:
  SNAPSHOT  the git revision to mutate, or None for the working tree
  FILES     [{"path": "src/...", "lines": "all" or "since:<revision>"}]
            "since:" mutates only the lines added or changed after it
  WORKERS   copies that run at once (default 4)
  JOBS      parallel jobs of each build and test run (default 3)
"""
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
WORK = os.path.join(os.path.expanduser("~"), ".cache",
                    "siliconmirror-mutation")

# --- Finding the code in a C file ------------------------------------------


def mask(text):
	"""text with comments, literals and preprocessor lines blanked.

	Every character keeps its offset; newlines stay. Of a #define, the
	value stays, so that its constants and operators are mutated.
	"""
	out = list(text)
	i, n = 0, len(text)
	line_start = True
	while i < n:
		c = text[i]
		if line_start and c in " \t":
			i += 1
			continue
		if line_start and c == "#":
			# The whole directive, with its continuation lines.
			end = i
			while True:
				nl = text.find("\n", end)
				if nl < 0:
					nl = n
				if nl > 0 and text[nl - 1] == "\\" and nl < n:
					end = nl + 1
					continue
				break
			directive = text[i:nl]
			m = re.match(r"#\s*define\s+\w+(\([^)]*\))?", directive)
			keep_from = i + m.end() if m else nl
			for k in range(i, keep_from):
				if out[k] != "\n":
					out[k] = " "
			# Comments inside the value are blanked by the loop below.
			i = keep_from
			line_start = False
			continue
		line_start = c == "\n"
		if text.startswith("/*", i):
			end = text.find("*/", i + 2)
			end = n if end < 0 else end + 2
			for k in range(i, end):
				if out[k] != "\n":
					out[k] = " "
			i = end
			continue
		if text.startswith("//", i):
			end = text.find("\n", i)
			end = n if end < 0 else end
			for k in range(i, end):
				out[k] = " "
			i = end
			continue
		if c in "\"'":
			k = i + 1
			while k < n and text[k] != c:
				k += 2 if text[k] == "\\" else 1
			for j in range(i + 1, min(k, n)):
				if out[j] != "\n":
					out[j] = " "
			i = k + 1
			continue
		if c == "\\" and i + 1 < n and text[i + 1] == "\n":
			out[i] = " "
		i += 1
	return "".join(out)


TOKEN = re.compile(r"""
	(?P<num>0[xX][0-9a-fA-F]+[uUlL]*|[0-9]+[uUlL]*)
	|(?P<id>[A-Za-z_]\w*)
	|(?P<punct><<=|>>=|\.\.\.|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||\+=|-=
	   |\*=|/=|%=|&=|\|=|\^=|[-+*/%&|^~!<>=?:;,.(){}\[\]])
	|(?P<space>\s+)
	|(?P<other>.)
""", re.X | re.S)

KEYWORDS = {
	"return", "if", "else", "while", "for", "do", "switch", "case",
	"default", "break", "continue", "goto", "sizeof", "static", "const",
	"struct", "union", "enum", "typedef", "extern", "volatile", "inline",
	"unsigned", "signed", "int", "char", "short", "long", "void", "bool",
	"float", "double", "_Bool", "static_assert", "_Static_assert",
}
TYPE_WORDS = {
	"static", "const", "struct", "union", "enum", "unsigned", "signed",
	"int", "char", "short", "long", "void", "bool", "float", "double",
	"volatile", "extern", "typedef",
}


def tokens(masked):
	"""(kind, text, start, end) of every token but spaces."""
	result = []
	for m in TOKEN.finditer(masked):
		kind = m.lastgroup
		if kind != "space":
			result.append((kind, m.group(), m.start(), m.end()))
	return result


def operand_before(toks, k):
	"""Whether the token before toks[k] ends an operand."""
	if k == 0:
		return False
	kind, text = toks[k - 1][0], toks[k - 1][1]
	if kind == "num":
		return True
	if kind == "id":
		return text not in KEYWORDS
	return text in (")", "]", "++", "--")


def pointer_declarator(toks, k):
	"""Whether the * at toks[k] follows a type name: Type *name."""
	kind, text = toks[k - 1][0], toks[k - 1][1]
	return kind == "id" and (text in TYPE_WORDS or text.endswith("_t")
	                         or (text[0].isupper() and not text.isupper()))


REPLACE_ALWAYS = {
	"==": ["!="], "!=": ["=="],
	"<": ["<=", ">="], "<=": ["<", ">"],
	">": [">=", "<="], ">=": [">", "<"],
	"&&": ["||"], "||": ["&&"],
	"<<": [">>"], ">>": ["<<"],
	"+=": ["-="], "-=": ["+="], "*=": ["/="], "/=": ["*="],
	"|=": ["&="], "&=": ["|="], "^=": ["|="],
	"<<=": [">>="], ">>=": ["<<="],
	"++": ["--"], "--": ["++"],
	"true": ["false"], "false": ["true"],
}
REPLACE_BINARY = {
	"+": ["-"], "-": ["+"], "*": ["/"], "/": ["*"], "%": ["/"],
	"&": ["|"], "|": ["&"], "^": ["|", "&"],
}


def number_mutants(text):
	"""The literals that replace the integer literal text."""
	m = re.match(r"(0[xX])?([0-9a-fA-F]+)([uUlL]*)$", text)
	prefix, digits, suffix = m.group(1), m.group(2), m.group(3)
	value = int(digits, 16 if prefix else 10)
	if value == 0:
		values = [1]
	elif value == 1:
		values = [0, 2]
	else:
		values = [value + 1, value - 1]
	result = []
	for v in values:
		if prefix:
			d = format(v, "x")
			if digits != digits.lower():
				d = d.upper()
			result.append(prefix + d + suffix)
		elif digits.startswith("0") and len(digits) > 1:
			continue  # an octal literal: leave it
		else:
			result.append(str(v) + suffix)
	return result


def matching(masked, open_at, open_char, close_char):
	"""The offset of the bracket that closes the one at open_at."""
	depth = 0
	for i in range(open_at, len(masked)):
		if masked[i] == open_char:
			depth += 1
		elif masked[i] == close_char:
			depth -= 1
			if depth == 0:
				return i
	return -1


def statements(masked, toks):
	"""(start, end) of every statement in a function body, ending at ;."""
	result = []
	braces = []  # "body", "block" or "init" for each open brace
	owners = []  # the token before each open parenthesis
	closed_owner = None  # that of the parenthesis that closed last
	last = None  # where the current statement starts
	for k, (kind, text, start, end) in enumerate(toks):
		prev = toks[k - 1][1] if k > 0 else None
		if text == "(":
			owners.append(prev)
		elif text == ")":
			closed_owner = owners.pop() if owners else None
		elif text == "{":
			if not braces:
				what = "body" if prev == ")" else "init"
			elif "init" in braces:
				what = "init"
			elif prev == ")" and closed_owner in ("if", "while", "for",
			                                      "switch"):
				what = "block"
			elif prev in ("else", "do", "{", "}", ";", ":"):
				what = "block"
			else:
				what = "init"  # an initializer or a compound literal
			braces.append(what)
			if what != "init":
				last = end
		elif text == "}":
			if (braces.pop() if braces else None) != "init":
				last = end
		elif (not braces or braces[0] != "body" or "init" in braces
		      or owners):
			continue
		elif text == ";":
			if last is not None:
				result.append((last, end))
			last = end
		elif text == ":" and last is not None:
			head = masked[last:start].strip()
			if (re.match(r"(case\b|default$)", head)
			        or re.match(r"[A-Za-z_]\w*$", head)):
				last = end  # a label ends here
	return result


def mutants_of(text):
	"""(start, end, replacement, kind) of every mutant of a C file."""
	masked = mask(text)
	toks = tokens(masked)
	result = []
	for k, (kind, tok, start, end) in enumerate(toks):
		if kind == "num":
			for r in number_mutants(tok):
				result.append((start, end, r, "constant"))
		elif tok in REPLACE_ALWAYS:
			for r in REPLACE_ALWAYS[tok]:
				result.append((start, end, r, "operator"))
		elif tok == "*" and pointer_declarator(toks, k):
			continue
		elif tok in REPLACE_BINARY and operand_before(toks, k):
			for r in REPLACE_BINARY[tok]:
				result.append((start, end, r, "operator"))
		elif tok in ("!", "~"):
			result.append((start, end, "", "operator"))
		elif tok == "-" and not operand_before(toks, k):
			result.append((start, end, "", "operator"))
		if tok in ("if", "while") and k + 1 < len(toks) \
		        and toks[k + 1][1] == "(":
			o = toks[k + 1][2]
			c = matching(masked, o, "(", ")")
			if c > 0:
				result.append((o, c + 1, "(!" + text[o:c + 1] + ")",
				               "condition"))
	for start, end in statements(masked, toks):
		body = masked[start:end]
		lead = len(body) - len(body.lstrip())
		s = start + lead
		words = re.findall(r"[A-Za-z_]\w*|\S", masked[s:end])
		if words == [";"]:
			continue  # an empty statement
		first = words[0]
		if first in TYPE_WORDS:
			continue  # a declaration
		if (first not in KEYWORDS and len(words) > 1
		        and re.match(r"[A-Za-z_]", words[1])):
			continue  # a declaration: a type, then a name
		if (first not in KEYWORDS and len(words) > 3 and words[1] == "*"
		        and re.match(r"[A-Za-z_]", words[2])
		        and words[3] in ("=", ";", ",", "[")):
			continue  # a pointer declaration
		if first == "return" and words[1:2] != [";"]:
			continue  # the other mutants change the value
		result.append((s, end, ";", "delete"))
		if first in ("if", "while", "for"):
			o = masked.find("(", s)
			c = matching(masked, o, "(", ")")
			if 0 < c < end - 1 and masked[c + 1:end - 1].strip():
				result.append((c + 1, end, " ;", "delete body"))
		elif first == "else":
			result.append((s + 4, end, " ;", "delete body"))
	return result


def line_of(text, offset):
	return text.count("\n", 0, offset) + 1


def describe(text, start, end, replacement, kind):
	"""The line of a mutant as it reads after the change; what it deletes."""
	if kind.startswith("delete"):
		return " ".join(text[start:end].split())
	ls = text.rfind("\n", 0, start) + 1
	le = text.find("\n", end)
	le = len(text) if le < 0 else le
	changed = text[ls:start] + replacement + text[end:le]
	return " ".join(changed.split())


def insn_table_lines(text):
	"""Lines of the sorted instruction table, checked against binutils."""
	if "insns[] = {" not in text:
		return set()
	head = text.index("insns[] = {")
	first = text[:head].count("\n") + 1
	last = first + text[head:].split("};")[0].count("\n")
	return set(range(first, last + 1))


# --- Choosing the lines ----------------------------------------------------


def git(*args):
	return subprocess.run(["git", "-C", REPO] + list(args), check=True,
	                      capture_output=True, text=True).stdout


def file_text(snapshot, path):
	if snapshot is None:
		return open(os.path.join(REPO, path)).read()
	return git("show", f"{snapshot}:{path}")


def chosen_lines(snapshot, path, lines, text):
	"""The line numbers of path that are mutated."""
	every = set(range(1, text.count("\n") + 2))
	if lines == "all":
		return every
	since = lines.split(":", 1)[1]
	if subprocess.run(["git", "-C", REPO, "cat-file", "-e",
	                   f"{since}:{path}"], capture_output=True).returncode:
		return every  # a new file
	rev = [since] if snapshot is None else [since, snapshot]
	diff = git("diff", "-U0", *rev, "--", path)
	result = set()
	for m in re.finditer(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@", diff, re.M):
		first, count = int(m.group(1)), int(m.group(2) or "1")
		result.update(range(first, first + count))
	return result


def all_mutants(config):
	"""Every mutant: (path, line, start, end, replacement, description)."""
	result = []
	for f in config["FILES"]:
		text = file_text(config.get("SNAPSHOT"), f["path"])
		wanted = (chosen_lines(config.get("SNAPSHOT"), f["path"],
		                       f["lines"], text)
		          - insn_table_lines(text))
		for start, end, rep, kind in mutants_of(text):
			n = line_of(text, start)
			if n in wanted:
				d = f"{kind}: {describe(text, start, end, rep, kind)}"
				result.append((f["path"], n, start, end, rep, d))
	return result


def survivors(config, log):
	"""The mutants that survived in an earlier run's log."""
	every = {}
	for m in all_mutants_unfiltered(config):
		every.setdefault((m[0], m[5]), []).append(m)
	result = []
	for line in open(log):
		m = re.match(r"SURVIVED (\S+):(\d+): (.*)$", line.rstrip("\n"))
		if not m:
			continue
		path, n, d = m.group(1), int(m.group(2)), m.group(3)
		found = sorted(every.get((path, d), []), key=lambda x: abs(x[1] - n))
		if found and abs(found[0][1] - n) <= 10:
			result.append(found[0])
		else:
			print(f"gone: {path}:{n}: {d}")
	return result


def all_mutants_unfiltered(config):
	result = []
	for f in config["FILES"]:
		text = file_text(config.get("SNAPSHOT"), f["path"])
		for start, end, rep, kind in mutants_of(text):
			d = f"{kind}: {describe(text, start, end, rep, kind)}"
			result.append((f["path"], line_of(text, start), start, end, rep,
			               d))
	return result


# --- Running ---------------------------------------------------------------


def run(cmd, cwd, timeout):
	try:
		return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
		                      timeout=timeout, stdin=subprocess.DEVNULL)
	except subprocess.TimeoutExpired:
		return None


def stage(snapshot):
	"""A directory with the files to mutate: the snapshot or the tree."""
	if snapshot is None:
		return REPO, None
	tmp = tempfile.mkdtemp(prefix="stage-", dir=WORK)
	archive = subprocess.run(["git", "-C", REPO, "archive", snapshot],
	                         check=True, capture_output=True).stdout
	subprocess.run(["tar", "-x", "-C", tmp], input=archive, check=True)
	return tmp, tmp


def prepare(w, source, jobs):
	"""Copy w of the source, built and checked against every test."""
	copy = os.path.join(WORK, f"w{w}")
	os.makedirs(copy, exist_ok=True)
	# Without -t, a file that changes gets a new time, so that the build
	# sees it; --checksum leaves the others alone.
	subprocess.run(["rsync", "-rlpD", "--checksum", "--delete",
	                "--exclude", "/build/", "--exclude", "/.git/",
	                source + "/", copy + "/"], check=True)
	build = os.path.join(copy, "build")
	if not os.path.exists(os.path.join(build, "CMakeCache.txt")):
		subprocess.run(["cmake", "-S", copy, "-B", build, "-G", "Ninja",
		                "-DCMAKE_BUILD_TYPE=Debug", "-DSMIRROR_SANITIZE=ON"],
		               check=True, capture_output=True)
	b = run(["cmake", "--build", build, "-j", str(jobs)], copy, 1800)
	assert b and b.returncode == 0, f"copy {w}: the build fails"
	t = run(["ctest", "--test-dir", build, "-j", str(jobs)], copy, 900)
	assert t and t.returncode == 0, f"copy {w}: the tests fail"
	return copy, build


def outcome(result):
	"""killed, survived or slow, from a run of ctest."""
	if result is None:
		return "slow"
	if result.returncode == 0:
		return "survived"
	failed = re.findall(r"\*\*\*(\w+)|\((Failed|Subprocess aborted|"
	                    r"SEGFAULT|Child aborted|Exception)\)", result.stdout)
	kinds = {a or b for a, b in failed}
	if kinds and kinds <= {"Timeout"}:
		return "slow"
	return "killed"


def load_results(path):
	"""What an earlier, unfinished run found: {(path, line, desc): result}."""
	done = {}
	if os.path.exists(path):
		for line in open(path):
			m = re.match(r"(\w+) (\S+):(\d+): (.*)$", line.rstrip("\n"))
			if m:
				done[(m.group(2), int(m.group(3)), m.group(4))] = m.group(1)
	return done


def main():
	config = {}
	exec(open(sys.argv[1]).read(), config)
	if sys.argv[2:3] == ["--count"]:
		todo = all_mutants(config)
		for path in sorted({m[0] for m in todo}):
			print(f"{sum(m[0] == path for m in todo):6} {path}")
		print(f"{len(todo):6} mutants")
		return
	log_path = sys.argv[2]
	results_path = log_path + ".results"
	os.makedirs(WORK, exist_ok=True)
	todo = (survivors(config, sys.argv[3]) if len(sys.argv) > 3
	        else all_mutants(config))
	# Each result is written as it comes, so that a run stopped half way,
	# even by a power cut, goes on from where it was.
	done = load_results(results_path)
	retry = [m for m in todo if done.get((m[0], m[1], m[5])) == "SLOW"]
	todo = [m for m in todo if (m[0], m[1], m[5]) not in done]
	workers = config.get("WORKERS", 4)
	jobs = config.get("JOBS", 3)
	start_time = time.time()
	source, tmp = stage(config.get("SNAPSHOT"))
	copies = [None] * workers
	errors = []

	def prepare_copy(w):
		try:
			copies[w] = prepare(w, source, jobs)
		except Exception as e:  # noqa: BLE001 - reported below
			errors.append(e)

	try:
		threads = [threading.Thread(target=prepare_copy, args=(w,))
		           for w in range(workers)]
		for t in threads:
			t.start()
		for t in threads:
			t.join()
	finally:
		if tmp:
			shutil.rmtree(tmp)
	if errors:
		raise errors[0]
	work = queue.Queue()
	lock = threading.Lock()
	results = open(results_path, "a")
	# The slowest test takes about 2 s alone: 30 s is a hang, even on a
	# busy machine; alone, it gets 120 s.
	limits = {"test": 30, "all": 900}

	def record(result, m):
		"""Note a result. Called with the lock held."""
		done[(m[0], m[1], m[5])] = result
		results.write(f"{result} {m[0]}:{m[1]}: {m[5]}\n")
		results.flush()
		os.fsync(results.fileno())
		counts = {}
		for r in done.values():
			counts[r] = counts.get(r, 0) + 1
		with open(log_path + ".progress", "w") as p:
			p.write(", ".join(f"{v} {k.lower()}" for k, v in
			                  sorted(counts.items())) +
			        f", {work.qsize()} waiting, "
			        f"{int(time.time() - start_time)} s\n")

	def worker(copy, build):
		while True:
			try:
				m = work.get_nowait()
			except queue.Empty:
				return
			path, n, start, end, rep, d = m
			full = os.path.join(copy, path)
			original = open(full).read()
			try:
				open(full, "w").write(original[:start] + rep + original[end:])
				b = run(["cmake", "--build", build, "-j", str(jobs)], copy, 900)
				if b is None or b.returncode != 0:
					with lock:
						record("UNCOMPILED", m)
					continue
				# The first failure kills the mutant: the rest need not
				# run. A survivor runs every test.
				t = run(["ctest", "--test-dir", build, "-j", str(jobs),
				         "--stop-on-failure",
				         "--timeout", str(limits["test"])], copy,
				        limits["all"])
				result = {"slow": "SLOW", "killed": "KILLED",
				          "survived": "SURVIVED"}[outcome(t)]
				if result == "SLOW" and limits["test"] > 30:
					result = "ENDLESS"  # out of time alone: a kill
				with lock:
					record(result, m)
			finally:
				open(full, "w").write(original)

	def phase(items, n_workers):
		for item in items:
			work.put(item)
		threads = [threading.Thread(target=worker, args=c)
		           for c in copies[:n_workers]]
		for t in threads:
			t.start()
		for t in threads:
			t.join()

	phase(todo, workers)
	# Tests that ran out of time while the machine was busy: again, alone,
	# with more time.
	retry += [m for m in todo if done.get((m[0], m[1], m[5])) == "SLOW"]
	limits["test"], limits["all"] = 120, 3600
	phase(retry, 1)
	results.close()
	final = {k: v for k, v in done.items()}
	counts = {}
	for r in final.values():
		counts[r] = counts.get(r, 0) + 1
	killed = counts.get("KILLED", 0) + counts.get("ENDLESS", 0)
	run_count = killed + counts.get("SURVIVED", 0)
	with open(log_path, "w") as log:
		for (path, n, d), r in sorted(final.items()):
			if r in ("SURVIVED", "ENDLESS", "SLOW"):
				log.write(f"{r} {path}:{n}: {d}\n")
		log.write(f"DONE {killed}/{run_count} killed "
		          f"({counts.get('ENDLESS', 0)} by an endless loop), "
		          f"{counts.get('UNCOMPILED', 0)} did not compile, "
		          f"{counts.get('SLOW', 0)} still slow, in "
		          f"{int(time.time() - start_time)} s\n")


main()
