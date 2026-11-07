#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Export the editor-visible property model, not a dump of reflected fields. */
TSharedRef<FJsonObject> ExportBlueprintDetails(UObject* Object, bool bShowAdvanced);
