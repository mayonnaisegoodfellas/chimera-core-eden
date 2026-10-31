-- The frontend test's script for Chimera's window: in the project it was
-- opened with, go to a frame, then save the picture there and say where the
-- machine is. run-frontend.sh compares the picture with the one chimera-run
-- draws at the same frame of the same project.
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

local function finish(status, detail)
	writeAll(job.meta, table.concat({
		"status=" .. status,
		"detail=" .. (detail or ""),
		"frames=" .. emu.framecount(),
		"lag=" .. emu.lagcount(),
		"tastudio=" .. tostring(tastudio.engaged()),
	}, "\n") .. "\n")
	client.exit()
end

if emu.getsystemid() ~= "NSW" then
	finish("ERROR", "wrong system id: " .. tostring(emu.getsystemid()))
end
if emu.getcorename() ~= "Eden" then
	finish("ERROR", "wrong core: " .. tostring(emu.getcorename()))
end

pcall(function() client.speedmode(6400) end)
-- the picture as the core drew it, not the window with its messages on it
pcall(function() client.setscreenshotosd(false) end)

local frames = tonumber(job.frames) or 120
-- TAStudio opens paused, and a paused TAStudio does not step for a script's
-- frameadvance. A seek is what a person does, so it is what this does: ask for
-- the frame, then step until the machine is there.
if tastudio.engaged() then
	tastudio.setplayback(frames)
	local spins = 0
	while emu.framecount() < frames do
		emu.frameadvance()
		spins = spins + 1
		if spins % 100 == 0 then
			console.log(string.format("seek: at frame %d of %d", emu.framecount(), frames))
		end
	end
else
	while emu.framecount() < frames do
		emu.frameadvance()
	end
end

client.screenshot(job.shot)
finish("OK", "")
