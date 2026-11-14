-- The frontend test's script for a project opened again: what the greenzone
-- holds as it opens, then the picture at a frame, as tastudio-shot.lua.
--
-- The job is the file named by the MINIHAWK_JOB variable:
--   frames=<the frame to go to>
--   shot=<where to save the picture (PNG)>
--   meta=<where to write what happened (text)>

local function writeAll(path, data)
	local f = assert(io.open(path, "wb"))
	f:write(data)
	f:close()
end

local jobPath = os.getenv("MINIHAWK_JOB")
if jobPath == nil then
	error("MINIHAWK_JOB is not set")
end
local job = {}
for line in io.lines(jobPath) do
	local k, v = line:match("^([^=]+)=(.*)$")
	if k then job[k] = v end
end

local opened = {
	"states=" .. tostring(chimera.session.greenzoneStates),
	"greenEnd=" .. tostring(chimera.session.greenEnd),
}

pcall(function() client.speedmode(6400) end)
pcall(function() client.setscreenshotosd(false) end)

local frames = tonumber(job.frames) or 120
local started = os.time()
if tastudio.engaged() then
	tastudio.setplayback(frames)
end
while emu.framecount() < frames do
	emu.frameadvance()
end
client.screenshot(job.shot)
writeAll(job.meta, table.concat(opened, "\n") .. "\n" .. table.concat({
	"status=OK",
	"frames=" .. emu.framecount(),
	"seconds=" .. (os.time() - started),
	"tastudio=" .. tostring(tastudio.engaged()),
}, "\n") .. "\n")
client.exit()
