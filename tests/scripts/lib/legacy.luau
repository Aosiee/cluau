-- A plain Luau module: .luau files are compiled as they are, untranslated.
local function twice(n: number): number
	if n ~= n then
		return 0
	end
	return n * 2
end
return { twice = twice }
