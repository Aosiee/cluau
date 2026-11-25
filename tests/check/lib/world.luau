-- The types of a host's module, as `cluau check --module` reads them.
export type Entity = { name: string, health: number }
local m: any = nil
return m :: {
	spawn: (name: string) -> Entity,
	find: (name: string) -> Entity?,
}
