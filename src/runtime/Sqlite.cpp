// @std/sqlite: SQLite databases (the library built in). A row is a table of
// its columns by name; NULL is nil, a BLOB a buffer, an INTEGER or REAL a
// number, TEXT a string. Parameters are `?` (in order, after the SQL) or
// `:name` / `@name` / `$name` (one table of them).

#include "Std.h"

#include "lua.h"
#include "lualib.h"

#include <sqlite3.h>

#include <cstring>
#include <memory>
#include <string>

namespace cluau::runtime::detail {

	namespace {

		constexpr const char* kDatabaseMeta = "cluau.Database";
		constexpr const char* kStatementMeta = "cluau.Statement";

		// A connection, shared by its statements (it closes after the last).
		struct Connection
		{
			sqlite3* Db = nullptr;
			std::string Path;
			int Savepoints = 0; // nested transactions under way

			~Connection()
			{
				if (Db)
					sqlite3_close_v2(Db);
			}
		};

		struct DatabaseBox
		{
			std::shared_ptr<Connection>* Shared = nullptr;
		};

		struct StatementBox
		{
			std::shared_ptr<Connection>* Owner = nullptr;
			sqlite3_stmt* Statement = nullptr;
		};

		Connection& Open(lua_State* L, int index, const char* fn)
		{
			DatabaseBox* box = static_cast<DatabaseBox*>(luaL_checkudata(L, index, kDatabaseMeta));
			Connection& c = **box->Shared;
			if (!c.Db)
				luaL_error(L, "%s: the database is closed", fn);
			return c;
		}

		[[noreturn]] void Fail(lua_State* L, sqlite3* db, const char* fn) { luaL_error(L, "%s: %s", fn, sqlite3_errmsg(db)); }

		sqlite3_stmt* Prepare(lua_State* L, Connection& c, const char* sql, size_t len, const char* fn, const char** rest = nullptr)
		{
			sqlite3_stmt* statement = nullptr;
			if (sqlite3_prepare_v2(c.Db, sql, (int)len, &statement, rest) != SQLITE_OK)
				Fail(L, c.Db, fn);
			if (!statement && !rest)
				luaL_error(L, "%s: the SQL holds no statement", fn);
			return statement;
		}

		void BindValue(lua_State* L, sqlite3* db, sqlite3_stmt* statement, int slot, int index, const char* fn)
		{
			int rc = SQLITE_OK;
			switch (lua_type(L, index))
			{
			case LUA_TNIL: rc = sqlite3_bind_null(statement, slot); break;
			case LUA_TBOOLEAN: rc = sqlite3_bind_int(statement, slot, lua_toboolean(L, index)); break;
			case LUA_TNUMBER:
			{
				const double n = lua_tonumber(L, index);
				if (n == (double)(sqlite3_int64)n && n >= -9007199254740992.0 && n <= 9007199254740992.0)
					rc = sqlite3_bind_int64(statement, slot, (sqlite3_int64)n);
				else
					rc = sqlite3_bind_double(statement, slot, n);
				break;
			}
			case LUA_TSTRING:
			{
				size_t len = 0;
				const char* s = lua_tolstring(L, index, &len);
				rc = sqlite3_bind_text(statement, slot, s, (int)len, SQLITE_TRANSIENT);
				break;
			}
			case LUA_TBUFFER:
			{
				size_t len = 0;
				void* data = lua_tobuffer(L, index, &len);
				rc = sqlite3_bind_blob(statement, slot, data, (int)len, SQLITE_TRANSIENT);
				break;
			}
			default:
				luaL_error(L, "%s: parameter %d is a %s (a parameter is nil, a boolean, a number, a string or a buffer)", fn, slot,
					luaL_typename(L, index));
			}
			if (rc != SQLITE_OK)
				Fail(L, db, fn);
		}

		// The parameters from `first` on: values for `?`, or one table of
		// named ones (its keys without the `:`, `@` or `$`).
		void Bind(lua_State* L, sqlite3* db, sqlite3_stmt* statement, int first, const char* fn)
		{
			sqlite3_reset(statement);
			sqlite3_clear_bindings(statement);
			const int top = lua_gettop(L);
			const int wanted = sqlite3_bind_parameter_count(statement);
			if (top == first && lua_istable(L, first))
			{
				for (int slot = 1; slot <= wanted; slot++)
				{
					const char* name = sqlite3_bind_parameter_name(statement, slot);
					if (!name)
						luaL_error(L, "%s: parameter %d has no name, but the parameters are a table of names", fn, slot);
					lua_getfield(L, first, name + 1);
					BindValue(L, db, statement, slot, -1, fn);
					lua_pop(L, 1);
				}
				return;
			}
			const int given = top >= first ? top - first + 1 : 0;
			if (given != wanted)
				luaL_error(L, "%s: the SQL takes %d parameter(s), and %d were given", fn, wanted, given);
			for (int slot = 1; slot <= wanted; slot++)
				BindValue(L, db, statement, slot, first + slot - 1, fn);
		}

		// The row the statement stands on, as a table.
		void PushRow(lua_State* L, sqlite3_stmt* statement)
		{
			const int columns = sqlite3_column_count(statement);
			lua_createtable(L, 0, columns);
			for (int i = 0; i < columns; i++)
			{
				switch (sqlite3_column_type(statement, i))
				{
				case SQLITE_NULL: continue;
				case SQLITE_INTEGER: lua_pushnumber(L, (double)sqlite3_column_int64(statement, i)); break;
				case SQLITE_FLOAT: lua_pushnumber(L, sqlite3_column_double(statement, i)); break;
				case SQLITE_BLOB:
				{
					const void* data = sqlite3_column_blob(statement, i);
					const int len = sqlite3_column_bytes(statement, i);
					void* out = lua_newbuffer(L, (size_t)len);
					if (len > 0)
						std::memcpy(out, data, (size_t)len);
					break;
				}
				default:
				{
					const unsigned char* text = sqlite3_column_text(statement, i);
					lua_pushlstring(L, reinterpret_cast<const char*>(text), (size_t)sqlite3_column_bytes(statement, i));
				}
				}
				lua_setfield(L, -2, sqlite3_column_name(statement, i));
			}
		}

		// Steps to the next row: true on one, false at the end.
		bool Step(lua_State* L, sqlite3* db, sqlite3_stmt* statement, const char* fn)
		{
			const int rc = sqlite3_step(statement);
			if (rc == SQLITE_ROW)
				return true;
			if (rc == SQLITE_DONE)
				return false;
			const std::string message = sqlite3_errmsg(db);
			sqlite3_reset(statement);
			luaL_error(L, "%s: %s", fn, message.c_str());
			return false;
		}

		// What a statement run with its parameters gives: every row, the
		// first, or the changes it made.
		enum class Want { All, First, Changes };

		int RunStatement(lua_State* L, sqlite3* db, sqlite3_stmt* statement, int first, Want want, const char* fn)
		{
			Bind(L, db, statement, first, fn);
			if (want == Want::All)
			{
				lua_newtable(L);
				int n = 0;
				while (Step(L, db, statement, fn))
				{
					PushRow(L, statement);
					lua_rawseti(L, -2, ++n);
				}
			}
			else if (want == Want::First)
			{
				if (Step(L, db, statement, fn))
					PushRow(L, statement);
				else
					lua_pushnil(L);
			}
			else
			{
				while (Step(L, db, statement, fn))
				{
				}
				lua_createtable(L, 0, 2);
				lua_pushnumber(L, (double)sqlite3_changes64(db));
				lua_setfield(L, -2, "changes");
				lua_pushnumber(L, (double)sqlite3_last_insert_rowid(db));
				lua_setfield(L, -2, "lastId");
			}
			sqlite3_reset(statement);
			return 1;
		}

		// A statement made for one call, finalized however the call ends.
		struct Once
		{
			sqlite3_stmt* Statement;
			~Once() { sqlite3_finalize(Statement); }
		};

		int DatabaseQuery(lua_State* L)
		{
			Connection& c = Open(L, 1, "db:query");
			size_t len = 0;
			const char* sql = luaL_checklstring(L, 2, &len);
			Once once{ Prepare(L, c, sql, len, "db:query") };
			return RunStatement(L, c.Db, once.Statement, 3, Want::All, "db:query");
		}

		int DatabaseFirst(lua_State* L)
		{
			Connection& c = Open(L, 1, "db:first");
			size_t len = 0;
			const char* sql = luaL_checklstring(L, 2, &len);
			Once once{ Prepare(L, c, sql, len, "db:first") };
			return RunStatement(L, c.Db, once.Statement, 3, Want::First, "db:first");
		}

		int DatabaseRun(lua_State* L)
		{
			Connection& c = Open(L, 1, "db:run");
			size_t len = 0;
			const char* sql = luaL_checklstring(L, 2, &len);
			Once once{ Prepare(L, c, sql, len, "db:run") };
			return RunStatement(L, c.Db, once.Statement, 3, Want::Changes, "db:run");
		}

		// db:exec(sql): every statement in the text, without parameters or results.
		int DatabaseExec(lua_State* L)
		{
			Connection& c = Open(L, 1, "db:exec");
			char* error = nullptr;
			if (sqlite3_exec(c.Db, luaL_checkstring(L, 2), nullptr, nullptr, &error) != SQLITE_OK)
			{
				const std::string message = error ? error : sqlite3_errmsg(c.Db);
				sqlite3_free(error);
				luaL_error(L, "db:exec: %s", message.c_str());
			}
			return 0;
		}

		StatementBox* NewStatement(lua_State* L, const std::shared_ptr<Connection>& owner, sqlite3_stmt* statement)
		{
			StatementBox* box = static_cast<StatementBox*>(lua_newuserdatadtor(L, sizeof(StatementBox), [](lua_State*, void* p) {
				StatementBox* box = static_cast<StatementBox*>(p);
				if (box->Statement)
					sqlite3_finalize(box->Statement);
				delete box->Owner;
			}));
			box->Owner = new std::shared_ptr<Connection>(owner);
			box->Statement = statement;
			luaL_getmetatable(L, kStatementMeta);
			lua_setmetatable(L, -2);
			return box;
		}

		// The iterator rows() gives: its statement (upvalue 1) steps on.
		int RowsNext(lua_State* L)
		{
			StatementBox* box = static_cast<StatementBox*>(lua_touserdata(L, lua_upvalueindex(1)));
			if (!box->Statement || !(*box->Owner)->Db)
				return 0;
			if (!Step(L, (*box->Owner)->Db, box->Statement, "rows"))
			{
				sqlite3_reset(box->Statement);
				return 0;
			}
			PushRow(L, box->Statement);
			return 1;
		}

		int DatabaseRows(lua_State* L)
		{
			Connection& c = Open(L, 1, "db:rows");
			DatabaseBox* db = static_cast<DatabaseBox*>(lua_touserdata(L, 1));
			size_t len = 0;
			const char* sql = luaL_checklstring(L, 2, &len);
			Once guard{ Prepare(L, c, sql, len, "db:rows") };
			Bind(L, c.Db, guard.Statement, 3, "db:rows");
			sqlite3_stmt* statement = guard.Statement;
			guard.Statement = nullptr; // the iterator's now
			NewStatement(L, *db->Shared, statement);
			lua_pushcclosure(L, RowsNext, "rows", 1);
			return 1;
		}

		int DatabasePrepare(lua_State* L)
		{
			Connection& c = Open(L, 1, "db:prepare");
			DatabaseBox* db = static_cast<DatabaseBox*>(lua_touserdata(L, 1));
			size_t len = 0;
			const char* sql = luaL_checklstring(L, 2, &len);
			NewStatement(L, *db->Shared, Prepare(L, c, sql, len, "db:prepare"));
			return 1;
		}

		// db:transaction(fn, ...): fn(...) between BEGIN and COMMIT (a
		// savepoint when one is under way); an error rolls it back and is
		// raised again. Returns what fn returned.
		int DatabaseTransaction(lua_State* L)
		{
			Connection& c = Open(L, 1, "db:transaction");
			luaL_checktype(L, 2, LUA_TFUNCTION);
			const std::string name = "cluau_" + std::to_string(c.Savepoints + 1);
			auto exec = [&](const std::string& sql) {
				char* error = nullptr;
				const int rc = sqlite3_exec(c.Db, sql.c_str(), nullptr, nullptr, &error);
				std::string message = rc == SQLITE_OK ? std::string() : error ? error : sqlite3_errmsg(c.Db);
				sqlite3_free(error);
				return message;
			};
			if (std::string error = exec("SAVEPOINT " + name); !error.empty())
				luaL_error(L, "db:transaction: %s", error.c_str());
			c.Savepoints++;
			// fn at 2, its arguments after it.
			const int status = lua_pcall(L, lua_gettop(L) - 2, LUA_MULTRET, 0);
			c.Savepoints--;
			if (status != LUA_OK)
			{
				exec("ROLLBACK TO " + name);
				exec("RELEASE " + name);
				lua_error(L);
			}
			if (std::string error = exec("RELEASE " + name); !error.empty())
				luaL_error(L, "db:transaction: %s", error.c_str());
			return lua_gettop(L) - 1;
		}

		int DatabaseClose(lua_State* L)
		{
			DatabaseBox* box = static_cast<DatabaseBox*>(luaL_checkudata(L, 1, kDatabaseMeta));
			Connection& c = **box->Shared;
			if (c.Db)
			{
				sqlite3_close_v2(c.Db);
				c.Db = nullptr;
			}
			return 0;
		}

		int DatabaseIndex(lua_State* L)
		{
			DatabaseBox* box = static_cast<DatabaseBox*>(luaL_checkudata(L, 1, kDatabaseMeta));
			const char* key = luaL_checkstring(L, 2);
			if (std::strcmp(key, "path") == 0)
			{
				lua_pushstring(L, (*box->Shared)->Path.c_str());
				return 1;
			}
			lua_getmetatable(L, 1);
			lua_getfield(L, -1, "methods");
			lua_getfield(L, -1, key);
			return 1;
		}

		StatementBox& CheckStatement(lua_State* L, const char* fn)
		{
			StatementBox* box = static_cast<StatementBox*>(luaL_checkudata(L, 1, kStatementMeta));
			if (!box->Statement || !(*box->Owner)->Db)
				luaL_error(L, "%s: the statement (or its database) is closed", fn);
			return *box;
		}

		int StatementQuery(lua_State* L)
		{
			StatementBox& s = CheckStatement(L, "statement:query");
			return RunStatement(L, (*s.Owner)->Db, s.Statement, 2, Want::All, "statement:query");
		}

		int StatementFirst(lua_State* L)
		{
			StatementBox& s = CheckStatement(L, "statement:first");
			return RunStatement(L, (*s.Owner)->Db, s.Statement, 2, Want::First, "statement:first");
		}

		int StatementRun(lua_State* L)
		{
			StatementBox& s = CheckStatement(L, "statement:run");
			return RunStatement(L, (*s.Owner)->Db, s.Statement, 2, Want::Changes, "statement:run");
		}

		int StatementRows(lua_State* L)
		{
			StatementBox& s = CheckStatement(L, "statement:rows");
			Bind(L, (*s.Owner)->Db, s.Statement, 2, "statement:rows");
			lua_pushvalue(L, 1);
			lua_pushcclosure(L, RowsNext, "rows", 1);
			return 1;
		}

		int StatementColumns(lua_State* L)
		{
			StatementBox& s = CheckStatement(L, "statement:columns");
			const int n = sqlite3_column_count(s.Statement);
			lua_createtable(L, n, 0);
			for (int i = 0; i < n; i++)
			{
				lua_pushstring(L, sqlite3_column_name(s.Statement, i));
				lua_rawseti(L, -2, i + 1);
			}
			return 1;
		}

		int StatementClose(lua_State* L)
		{
			StatementBox* box = static_cast<StatementBox*>(luaL_checkudata(L, 1, kStatementMeta));
			if (box->Statement)
			{
				sqlite3_finalize(box->Statement);
				box->Statement = nullptr;
			}
			return 0;
		}

		// sqlite.open(path, options?): ":memory:" for a database in memory;
		// options.readonly, options.create (true by default).
		int SqliteOpen(lua_State* L)
		{
			const std::string path = luaL_checkstring(L, 1);
			bool readonly = false, create = true;
			if (lua_istable(L, 2))
			{
				lua_getfield(L, 2, "readonly");
				readonly = lua_toboolean(L, -1);
				lua_getfield(L, 2, "create");
				create = lua_isnil(L, -1) || lua_toboolean(L, -1);
				lua_pop(L, 2);
			}
			const int flags = (readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0)) | SQLITE_OPEN_URI;
			auto connection = std::make_shared<Connection>();
			connection->Path = path;
			if (sqlite3_open_v2(path.c_str(), &connection->Db, flags, nullptr) != SQLITE_OK)
			{
				const std::string message = connection->Db ? sqlite3_errmsg(connection->Db) : "out of memory";
				luaL_error(L, "sqlite.open: cannot open '%s': %s", path.c_str(), message.c_str());
			}
			sqlite3_busy_timeout(connection->Db, 5000);
			sqlite3_exec(connection->Db, "PRAGMA foreign_keys = ON", nullptr, nullptr, nullptr);
			DatabaseBox* box = static_cast<DatabaseBox*>(lua_newuserdatadtor(L, sizeof(DatabaseBox), [](lua_State*, void* p) {
				delete static_cast<DatabaseBox*>(p)->Shared;
			}));
			box->Shared = new std::shared_ptr<Connection>(std::move(connection));
			luaL_getmetatable(L, kDatabaseMeta);
			lua_setmetatable(L, -2);
			return 1;
		}

		void Type(lua_State* L, const char* meta, const char* name, lua_CFunction index, const luaL_Reg* methods)
		{
			luaL_newmetatable(L, meta);
			lua_pushstring(L, name);
			lua_setfield(L, -2, "__type");
			lua_newtable(L);
			for (const luaL_Reg* m = methods; m->name; m++)
			{
				lua_pushcfunction(L, m->func, m->name);
				lua_setfield(L, -2, m->name);
			}
			if (index)
			{
				lua_setfield(L, -2, "methods");
				lua_pushcfunction(L, index, "index");
			}
			lua_setfield(L, -2, "__index");
			lua_pop(L, 1);
		}

	} // namespace

	void OpenSqlite(lua_State* L)
	{
		const luaL_Reg database[] = { { "exec", DatabaseExec }, { "query", DatabaseQuery }, { "first", DatabaseFirst }, { "run", DatabaseRun },
			{ "rows", DatabaseRows }, { "prepare", DatabasePrepare }, { "transaction", DatabaseTransaction }, { "close", DatabaseClose },
			{ nullptr, nullptr } };
		const luaL_Reg statement[] = { { "query", StatementQuery }, { "first", StatementFirst }, { "run", StatementRun },
			{ "rows", StatementRows }, { "columns", StatementColumns }, { "close", StatementClose }, { nullptr, nullptr } };
		Type(L, kDatabaseMeta, "Database", DatabaseIndex, database);
		Type(L, kStatementMeta, "Statement", nullptr, statement);

		lua_newtable(L);
		lua_pushcfunction(L, SqliteOpen, "open");
		lua_setfield(L, -2, "open");
		lua_pushstring(L, sqlite3_libversion());
		lua_setfield(L, -2, "version");
	}

} // namespace cluau::runtime::detail
