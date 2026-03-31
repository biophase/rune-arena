local sourceFile = app.params["source_file"]
local outputDir = app.params["output_dir"]
local sourceName = app.params["source_name"]

if not sourceFile or sourceFile == "" then
  error("Missing script param: source_file")
end

if not outputDir or outputDir == "" then
  error("Missing script param: output_dir")
end

if not sourceName or sourceName == "" then
  sourceName = "sprite"
end

local sprite = app.open(sourceFile)
if not sprite then
  error("Unable to open sprite: " .. sourceFile)
end

local function sanitizeName(name)
  local value = string.lower(name or "")
  value = value:gsub("%s+", "_")
  value = value:gsub("[^%w%._-]+", "_")
  value = value:gsub("_+", "_")
  value = value:gsub("^_+", "")
  value = value:gsub("_+$", "")
  if value == "" then
    value = "layer"
  end
  return value
end

local function joinPath(left, right)
  if left:sub(-1) == "/" then
    return left .. right
  end
  return left .. "/" .. right
end

local allLayers = {}
local exportLayers = {}

local function collectLayers(layers, pathParts, parents)
  for _, layer in ipairs(layers) do
    table.insert(allLayers, layer)

    local nextParents = {}
    for i, parent in ipairs(parents) do
      nextParents[i] = parent
    end

    local nextPathParts = {}
    for i, part in ipairs(pathParts) do
      nextPathParts[i] = part
    end

    if layer.isGroup then
      table.insert(nextParents, layer)
      table.insert(nextPathParts, sanitizeName(layer.name))
      collectLayers(layer.layers, nextPathParts, nextParents)
    elseif layer.isImage then
      local item = {
        layer = layer,
        parents = nextParents,
        pathParts = nextPathParts,
      }
      table.insert(item.pathParts, sanitizeName(layer.name))
      table.insert(exportLayers, item)
    end
  end
end

collectLayers(sprite.layers, {}, {})

if #exportLayers == 0 then
  error("No image layers found in sprite")
end

local originalVisibility = {}
for _, layer in ipairs(allLayers) do
  originalVisibility[layer] = layer.isVisible
  layer.isVisible = false
end

local usedNames = {}

for _, item in ipairs(exportLayers) do
  for _, layer in ipairs(allLayers) do
    layer.isVisible = false
  end

  for _, parent in ipairs(item.parents) do
    parent.isVisible = true
  end
  item.layer.isVisible = true

  local fileStem = sourceName .. "-" .. table.concat(item.pathParts, "_")
  if usedNames[fileStem] then
    usedNames[fileStem] = usedNames[fileStem] + 1
    fileStem = fileStem .. "_" .. usedNames[fileStem]
  else
    usedNames[fileStem] = 1
  end

  local textureFilename = joinPath(outputDir, fileStem .. ".png")
  local dataFilename = joinPath(outputDir, fileStem .. ".json")

  print("Exporting " .. item.layer.name .. " -> " .. fileStem)

  app.command.ExportSpriteSheet {
    ui = false,
    askOverwrite = false,
    type = SpriteSheetType.PACKED,
    textureFilename = textureFilename,
    dataFilename = dataFilename,
    dataFormat = SpriteSheetDataFormat.JSON_ARRAY,
    borderPadding = 0,
    shapePadding = 0,
    innerPadding = 0,
    trimSprite = false,
    trim = false,
    trimByGrid = false,
    extrude = false,
    ignoreEmpty = false,
    mergeDuplicates = false,
    openGenerated = false,
    listLayers = false,
    listTags = true,
    listSlices = false,
  }
end

for _, layer in ipairs(allLayers) do
  layer.isVisible = originalVisibility[layer]
end

app.command.CloseFile()
