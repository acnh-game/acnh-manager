#include "install/install_task.hpp"

#include "util/json.hpp"

namespace acnh_manager::install {
namespace {

const char *SourceName(TaskSource source) {
    switch (source) {
        case TaskSource::LocalFile: return "file";
        case TaskSource::HttpDownload: return "http-client";
        case TaskSource::HttpUpload: return "http-server";
        case TaskSource::None: break;
    }
    return "none";
}

TaskSource SourceFromName(const std::string &name) {
    if (name == "file") {
        return TaskSource::LocalFile;
    }
    if (name == "http-client") {
        return TaskSource::HttpDownload;
    }
    if (name == "http-server") {
        return TaskSource::HttpUpload;
    }
    return TaskSource::None;
}

const char *StageName(TaskStage stage) {
    switch (stage) {
        case TaskStage::Idle: return "idle";
        case TaskStage::Waiting: return "waiting";
        case TaskStage::Fetching: return "fetching";
        case TaskStage::Finalizing: return "finalizing";
        case TaskStage::Done: return "done";
        case TaskStage::Failed: return "failed";
    }
    return "idle";
}

TaskStage StageFromName(const std::string &name) {
    if (name == "waiting") {
        return TaskStage::Waiting;
    }
    if (name == "fetching") {
        return TaskStage::Fetching;
    }
    if (name == "finalizing") {
        return TaskStage::Finalizing;
    }
    if (name == "done") {
        return TaskStage::Done;
    }
    if (name == "failed") {
        return TaskStage::Failed;
    }
    return TaskStage::Idle;
}

const char *KindName(ContentKind kind) {
    switch (kind) {
        case ContentKind::Meta: return "meta";
        case ContentKind::Program: return "program";
        case ContentKind::Control: return "control";
        case ContentKind::Legal: return "legal";
        case ContentKind::Offline: return "offline";
        case ContentKind::Data: return "data";
        case ContentKind::PublicData: return "public-data";
        case ContentKind::DeltaFragment: return "delta";
        case ContentKind::Unknown: break;
    }
    return "unknown";
}

ContentKind KindFromName(const std::string &name) {
    if (name == "meta") {
        return ContentKind::Meta;
    }
    if (name == "program") {
        return ContentKind::Program;
    }
    if (name == "control") {
        return ContentKind::Control;
    }
    if (name == "legal") {
        return ContentKind::Legal;
    }
    if (name == "offline") {
        return ContentKind::Offline;
    }
    if (name == "data") {
        return ContentKind::Data;
    }
    if (name == "public-data") {
        return ContentKind::PublicData;
    }
    if (name == "delta") {
        return ContentKind::DeltaFragment;
    }
    return ContentKind::Unknown;
}

}  // namespace

std::uint64_t TotalBytes(const InstallTask &task) {
    std::uint64_t total = 0;
    for (const auto &content : task.contents) {
        if (!content.skipped) {
            total += content.size;
        }
    }
    return total;
}

std::uint64_t DoneBytes(const InstallTask &task) {
    std::uint64_t done = 0;
    for (const auto &content : task.contents) {
        if (content.skipped) {
            continue;
        }
        done += content.written < content.size ? content.written : content.size;
    }
    return done;
}

std::uint64_t PackageBytes(const InstallTask &task) {
    std::uint64_t total = 0;
    for (const auto &content : task.contents) {
        total += content.size;
    }
    return total;
}

int DoneContents(const InstallTask &task) {
    int done = 0;
    for (const auto &content : task.contents) {
        if (content.skipped || content.written >= content.size) {
            ++done;
        }
    }
    return done;
}

bool NextRange(const InstallTask &task, std::uint64_t max_size, RangeRequest *out) {
    if (out == nullptr || max_size == 0) {
        return false;
    }
    /* Container order: the first content that still needs bytes.  Sending the meta content first
       is the caller's job (it decides the order of `contents`), not this loop's. */
    for (std::size_t i = 0; i < task.contents.size(); ++i) {
        const TaskContent &content = task.contents[i];
        if (content.skipped || content.written >= content.size) {
            continue;
        }
        const std::uint64_t missing = content.size - content.written;
        out->index = i;
        out->offset = content.offset + content.written;
        out->size = missing < max_size ? missing : max_size;
        return true;
    }
    return false;
}

void Advance(InstallTask &task, const RangeRequest &range, std::uint64_t size) {
    if (range.index >= task.contents.size()) {
        return;
    }
    TaskContent &content = task.contents[range.index];
    const std::uint64_t missing = content.size - (content.written < content.size
                                                       ? content.written
                                                       : content.size);
    const std::uint64_t taken = size < missing ? size : missing;
    content.written += taken;
}

bool TransferComplete(const InstallTask &task) {
    for (const auto &content : task.contents) {
        if (!content.skipped && content.written < content.size) {
            return false;
        }
    }
    return true;
}

bool MatchesSourceFile(const InstallTask &task, std::string_view name, std::uint64_t size) {
    if (task.name != name) {
        return false;
    }
    /* A source that cannot report a size (0) is accepted: the content-level check still runs. */
    return task.container_size == 0 || size == 0 || task.container_size == size;
}

ResumePlan PlanResume(const TaskContent &recorded, const TaskContent &fresh,
                      bool placeholder_present) {
    ResumePlan plan;
    /* The file has to be talking about this very content: same id (all 32 hex digits) and same
       size.  A repacked package can reuse an id for different bytes, and the size is the cheapest
       honest check we have without hashing the whole content. */
    if (recorded.content_id.empty() || recorded.content_id != fresh.content_id) {
        return plan;
    }
    if (recorded.size != fresh.size) {
        return plan;
    }
    if (!placeholder_present || recorded.placeholder_id.empty()) {
        return plan;
    }
    if (fresh.compressed) {
        /* Rule 2: a zstd stream cannot be entered in the middle. */
        return plan;
    }
    /* Rule 1: the bytes are there, the content is the same one.  A `written` past the end is a
       corrupt file, not a licence to skip: clamp it. */
    plan.restart = false;
    plan.written = recorded.written > fresh.size ? fresh.size : recorded.written;
    plan.placeholder_id = recorded.placeholder_id;
    return plan;
}

std::string DumpTask(const InstallTask &task) {
    json::Value root;
    root.type = json::Type::Object;
    root.object.emplace_back("schema", json::Value::MakeNumber(1));
    root.object.emplace_back("taskId", json::Value::MakeString(task.task_id));
    root.object.emplace_back("source", json::Value::MakeString(SourceName(task.source)));
    root.object.emplace_back("name", json::Value::MakeString(task.name));
    root.object.emplace_back("url", json::Value::MakeString(task.url));
    root.object.emplace_back(
        "containerSize",
        json::Value::MakeNumber(static_cast<double>(task.container_size)));
    root.object.emplace_back("target", json::Value::MakeString(
                                           task.target == TargetStorage::Nand ? "nand" : "sd"));
    root.object.emplace_back("stage", json::Value::MakeString(StageName(task.stage)));
    root.object.emplace_back("error", json::Value::MakeString(task.error));

    json::Value contents;
    contents.type = json::Type::Array;
    for (const auto &content : task.contents) {
        json::Value node;
        node.type = json::Type::Object;
        node.object.emplace_back("name", json::Value::MakeString(content.name));
        node.object.emplace_back("offset",
                                 json::Value::MakeNumber(static_cast<double>(content.offset)));
        node.object.emplace_back("size", json::Value::MakeNumber(static_cast<double>(content.size)));
        node.object.emplace_back("written",
                                 json::Value::MakeNumber(static_cast<double>(content.written)));
        node.object.emplace_back("contentId", json::Value::MakeString(content.content_id));
        node.object.emplace_back("placeholderId", json::Value::MakeString(content.placeholder_id));
        node.object.emplace_back("kind", json::Value::MakeString(KindName(content.kind)));
        node.object.emplace_back("skipped", json::Value::MakeBool(content.skipped));
        contents.array.push_back(std::move(node));
    }
    root.object.emplace_back("contents", std::move(contents));
    return json::Dump(root);
}

bool ParseTask(std::string_view text, InstallTask *out, std::string *error) {
    error->clear();
    *out = InstallTask{};

    json::Value root;
    std::string json_error;
    if (!json::Parse(text, &root, &json_error) || !root.IsObject()) {
        *error = "install-task.json could not be parsed: " + json_error;
        return false;
    }
    const json::Value *schema = root.Find("schema");
    if (schema == nullptr || json::IntOr(*schema, 0) != 1) {
        *error = "install-task.json has an unknown schema";
        return false;
    }

    auto string_of = [&root](const char *key, const std::string &fallback) -> std::string {
        const json::Value *value = root.Find(key);
        return value != nullptr ? value->StringOr(fallback) : fallback;
    };
    auto number_of = [&root](const char *key) -> std::uint64_t {
        const json::Value *value = root.Find(key);
        const std::int64_t number = value != nullptr ? json::IntOr(*value, 0) : 0;
        return number > 0 ? static_cast<std::uint64_t>(number) : 0;
    };

    out->task_id = string_of("taskId", "");
    out->source = SourceFromName(string_of("source", "none"));
    out->name = string_of("name", "");
    out->url = string_of("url", "");
    out->container_size = number_of("containerSize");
    /* Older files have no target: SD is the default the settings carry as well, and a task whose
       storage is wrong is refused by the resume probe anyway (the placeholders are not there). */
    out->target = string_of("target", "sd") == "nand" ? TargetStorage::Nand : TargetStorage::SdCard;
    out->stage = StageFromName(string_of("stage", "idle"));
    out->error = string_of("error", "");

    const json::Value *contents = root.Find("contents");
    if (contents == nullptr || !contents->IsArray()) {
        *error = "install-task.json has no contents";
        return false;
    }
    for (std::size_t i = 0; i < contents->array.size(); ++i) {
        const json::Value &node = contents->array[i];
        if (!node.IsObject()) {
            *error = "install-task.json has a contents entry that is not an object";
            return false;
        }
        TaskContent content;
        auto field = [&node](const char *key) -> const json::Value * { return node.Find(key); };
        const json::Value *name = field("name");
        const json::Value *offset = field("offset");
        const json::Value *size = field("size");
        const json::Value *written = field("written");
        if (name == nullptr || offset == nullptr || size == nullptr || written == nullptr) {
            *error = "install-task.json has a contents entry missing a field";
            return false;
        }
        content.name = name->StringOr("");
        content.offset = static_cast<std::uint64_t>(json::IntOr(*offset, 0));
        content.size = static_cast<std::uint64_t>(json::IntOr(*size, 0));
        content.written = static_cast<std::uint64_t>(json::IntOr(*written, 0));
        if (content.written > content.size) {
            /* A hand-edited or half-written file must not make the installer ask for a range
               that cannot exist. */
            *error = "install-task.json has progress past the end of a content";
            return false;
        }
        const json::Value *id = field("contentId");
        if (id != nullptr) {
            content.content_id = id->StringOr("");
        }
        const json::Value *placeholder = field("placeholderId");
        if (placeholder != nullptr) {
            content.placeholder_id = placeholder->StringOr("");
        }
        const json::Value *kind = field("kind");
        if (kind != nullptr) {
            content.kind = KindFromName(kind->StringOr("unknown"));
        }
        const json::Value *skipped = field("skipped");
        if (skipped != nullptr) {
            content.skipped = skipped->BoolOr(false);
        }
        out->contents.push_back(std::move(content));
    }
    return true;
}

}  // namespace acnh_manager::install
