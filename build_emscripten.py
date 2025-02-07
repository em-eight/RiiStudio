from pathlib import Path
import glob, os, subprocess

import json
from hashlib import sha256

from shutil import copyfile

import sys
from itertools import chain

from multiprocessing.dummy import Pool as ThreadPool
import time

gPool = ThreadPool(4)

def require_dir(path):
	# TODO: if not isdir, delete
	if not os.path.exists(path):
		os.makedirs(path)
def system_cmd(cmd):
	# print(cmd)
	if subprocess.call(cmd, shell=True):
		print(cmd)
		raise SystemExit("Fail")



def hash(path):
		with open(path, "rb") as file:
			return sha256(file.read()).hexdigest()

class HashManager:
	def __init__(self, path):
		try:
			self.hashes = json.load(open(path, 'r'))
		except FileNotFoundError:
			self.hashes = {}
		self.path = path

	def hash(self, path):
		return hash(path)


	def check(self, path, cfg):
		return cfg+path in self.hashes and self.hashes[cfg+path] == self.hash(path)


	def save(self, path, cfg):
		self.hashes[cfg+path] = self.hash(path)

	def invalidate(self, path):
		print("path: %s" % path)
		if path in self.hashes:
			self.hashes[path] = ""
			print("[Hash Manager] invalidate %s" % path)
	def store_to_file(self):
		json.dump(self.hashes, open(self.path, 'w'))

gHashManager = HashManager("bin-int/hashes.json")


DEFINES = [
	"RII_PLATFORM_EMSCRIPTEN",
	"IMGUI_IMPL_OPENGL_ES2",
	"RII_BACKEND_SDL",
	"__linux__"
]
INCLUDES = [
	"source",
	"source/plate/include",
	"source/plate/vendor",
	"source/vendor/pybind11/include",
	"C:/Python36/include"
]

def defines(config):
	if config == "DEBUG":
		return DEFINES + [ "BUILD_DEBUG", "DEBUG" ]
	elif config == "RELEASE":
		return DEFINES + [ "BUILD_RELEASE", "NDEBUG" ]
	elif config == "DIST":
		return DEFINES + [ "BUILD_DIST", "NDEBUG" ]
	else:
		raise SystemExit("Unknown configuration")

def outputdir(config):
	return config.title() + "-emscripten-wasm"

PROJECTS = [
	{
		"name": "vendor",
		"type": "static_lib"
	},
	{
		"name": "core",
		"type": "static_lib"
	},
	{
		"name": "plugins",
		"type": "static_lib"
	},
	{
		"name": "librii",
		"type": "static_lib"
	},
	{
		"name": "plate",
		"type": "static_lib"
	},
	{
		"name": "oishii",
		"type": "static_lib"
	},
	{
		"name": "frontend",
		"type": "main_app",
		"links": [
			"vendor",
			"core",
			"plugins",
			"librii",
			"plate",
			"oishii"
		]
	}
]

def format_out(source, int_dir):
	return os.path.join(int_dir,
		source.replace(".cpp", ".o").replace(".cxx", ".o").replace(".c", ".o").replace("\\", "_").replace("/", "_")) 

def compile(source, int_dir, debug, config):
	print("BUILD %s" % source)
	args = "em++ -std=c++20 -Wno-deprecated-volatile -Wno-inconsistent-missing-override -Wno-ambiguous-reversed-operator -I./ "
	if source.endswith(".c"):
		args = args.replace("em++ -std=c++20", "emcc")
	for proj in PROJECTS:
		args += " -I./source/" + proj["name"] + " "
	for d in DEFINES + defines(config):
		args += " -D" + d
	for incl in INCLUDES:
		args += " -I./" + incl + " "
	args += " -Wall"
	#args += " -D\"__debugbreak()\"=\"\""
	args += " -s USE_SDL=2 "
	if debug:
		args += " -g4 -O0 "
	else:
		args += " -O3 -flto" 
	#args += " -s WASM=1"
	#args += " -s ALLOW_MEMORY_GROWTH=1"
	#args += " -s NO_EXIT_RUNTIME=0"
	#args += " -s ASSERTIONS=1"

	args += " -c -o"
	dst = format_out(source, int_dir)
	args += " " + dst + " " + source
	system_cmd(args)
	# Only if success:
	gHashManager.store_to_file()
	return dst

def build_project(name, type, config, proj=None):
	require_dir("bin/" + outputdir(config))
	require_dir("bin-int/" + outputdir(config))

	locals_objs = []

	debug = config.upper() == "DEBUG"

	bin_dir = "bin/" + outputdir(config) + "/" + name + "/"
	bin_int_dir = "bin-int/" + outputdir(config) + "/" + name + "/"
	
	require_dir(bin_dir)
	require_dir(bin_int_dir)

	get_sources = lambda src, filter: [str(x) for x in Path(src).glob(filter) ]

	cpp = get_sources("source/" + name, "**/*.cpp")
	cxx = get_sources("source/" + name, "**/*.cxx")
	c   = get_sources("source/" + name, "**/*.c")
	sources = chain(cpp, cxx, c)
	sources = list(filter(lambda x: "pybind11\\tests" not in x, sources))
	for source in sources:
		locals_objs.append(format_out(source, bin_int_dir))
	sources = list(filter(lambda x: not gHashManager.check(x, config), sources))
	do_compile = lambda source: [ compile(source, bin_int_dir, debug, config), gHashManager.save(source, config)]

	if len(sources):
		gPool.map(do_compile, sources)

	if type == "main_app":
		# Main app must be last
		gPool.close()
		gPool.join()
		print("LINKING")

		link_cmd = " -o " + bin_dir + "out.html -s USE_SDL=2 -s USE_FREETYPE=1 -s MAX_WEBGL_VERSION=2 -s DISABLE_DEPRECATED_FIND_EVENT_TARGET_BEHAVIOR=0 --bind"
		link_cmd += "  --shell-file " + bin_dir + "/shell_minimal.html "
		objs = locals_objs
		for lib in PROJECTS:
			if lib["name"] in proj["links"]:
				objs += lib["objs"]
		link_cmd += " ".join(objs)
		for a in ["libassimp", "libIrrXML", "libzlib", "libzlibstatic"]:
			link_cmd += " source/vendor/assimp/" + a + ".a"
		if debug:
			link_cmd += " -g4 --source-map-base ../ "
		link_cmd += "  -s WASM=1 -s ALLOW_MEMORY_GROWTH=1 -s NO_EXIT_RUNTIME=0 -s ASSERTIONS=1 --no-heap-copy --preload-file ./fonts@/fonts "
		link_cmd += " --preload-file ./lang@/lang"
		link_cmd += " --preload-file ./samp@/samp"
		with open(bin_int_dir + "link_cmd.txt", 'w') as lc:
			lc.write(link_cmd)
		system_cmd("em++ @" + bin_int_dir + "link_cmd.txt")
		copyfile("source/" + name + "/app.html", bin_dir + "/app.html")
	return locals_objs

def build_projects(config, rebuild):
	if rebuild:
		to_remove = []
		for hash in gHashManager.hashes:
			if not 'vendor' in hash:
				to_remove.append(hash)
		for hash in to_remove:
			gHashManager.hashes.pop(hash, None)
	for proj in PROJECTS:
		proj["objs"] = build_project(proj["name"], proj["type"], config, proj)

cfg = "RELEASE"
if len(sys.argv) > 1:
	cfg = sys.argv[1].upper()

start = time.time()
build_projects(cfg, len(sys.argv) > 2 and sys.argv[2] == "-rebuild")

gHashManager.store_to_file()
end = time.time()
print("Operation took %ss" % (end - start))