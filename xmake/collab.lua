-- collab.lua — shared helper for BuildWithCollab projects
--
-- Usage in xmake.lua:
--   includes("xmake/collab.lua")
--
--   -- Top level (like add_requires)
--   add_collab_requires("collab-core", "collab-process")
--
--   -- Inside a target (like add_packages)
--   target("myapp")
--       add_collab_packages("collab-core", "collab-process")
--
-- Environment variables:
--   BUILDWITHCOLLAB_ROOT   — path to the folder containing all collab repos
--                              (e.g. C:\Code\mrowr\BuildWithCollab)
--   BUILDWITHCOLLAB_LOCAL  — comma-delimited list of package names to pull
--                              from local folders instead of the xmake registry
--                              (e.g. "collab-core,collab-process")

local function _get_local_set()
    local root = os.getenv("BUILDWITHCOLLAB_ROOT")
    local local_csv = os.getenv("BUILDWITHCOLLAB_LOCAL")

    if not root or not os.isdir(root) then return {}, nil end
    if not local_csv or #local_csv == 0 then return {}, root end

    local set = {}
    for name in local_csv:gmatch("([^,]+)") do
        local trimmed = name:match("^%s*(.-)%s*$")
        if #trimmed > 0 then
            set[trimmed] = true
        end
    end
    return set, root
end

-- Top level — like add_requires, but checks local first
function add_collab_requires(...)
    local names = {...}
    local local_set, root = _get_local_set()

    local not_found = {}

    for _, name in ipairs(names) do
        if local_set[name] then
            local xmake_file = path.join(root, name, "xmake.lua")
            if os.isfile(xmake_file) then
                includes(xmake_file)
            else
                table.insert(not_found, name)
            end
        else
            add_requires(name)
        end
    end

    if #not_found > 0 then
        raise("BUILDWITHCOLLAB_LOCAL lists packages not found in "
            .. root .. ":\n  " .. table.concat(not_found, ", ")
            .. "\n\nClone them or remove from BUILDWITHCOLLAB_LOCAL.")
    end
end

-- Inside a target — like add_packages, but uses add_deps for local
function add_collab_packages(...)
    local names = {...}
    local local_set, root = _get_local_set()

    for _, name in ipairs(names) do
        if local_set[name] and root and os.isfile(path.join(root, name, "xmake.lua")) then
            add_deps(name, { public = true })
        else
            add_packages(name, { public = true })
        end
    end
end
