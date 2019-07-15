#include "./core/world.h"

#include <chrono>
#include <fstream>
#include <getopt.h>
#include <glm/gtc/matrix_transform.hpp>
#include <regex>
#include <set>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#include "./core/buffer_object.h"
#include "./core/link.h"
#include "./core/scene.h"
#include "./core/serializer.h"
#include "./image/image.h"
#include "./image/queue.h"
#include "./mesh/mesh.h"
#include "./utils/jsonutils.h"
#include "./utils/log.h"
#include "./utils/osutils.h"
#include "./utils/timer.h"

using namespace glm;
using namespace std;

#define SPLASH_CAMERA_LINK "__camera_link"

namespace Splash
{
/*************/
World* World::_that;

/*************/
World::World(int argc, char** argv)
{
    parseArguments(argc, argv);
    init();
}

/*************/
World::~World()
{
#ifdef DEBUG
    Log::get() << Log::DEBUGGING << "World::~World - Destructor" << Log::endl;
#endif
    if (_innerSceneThread.joinable())
        _innerSceneThread.join();
}

/*************/
void World::run()
{
    // If set to run as a child process, only create a scene which will wait for instructions
    // from the master process
    if (_runAsChild)
    {
        Log::get() << Log::MESSAGE << "World::" << __FUNCTION__ << " - Creating child Scene with name " << _childSceneName << Log::endl;

        Scene scene(_childSceneName, _linkSocketPrefix);
        scene.run();

        return;
    }

    applyConfig();

    while (true)
    {
        Timer::get() << "loop_world";
        Timer::get() << "loop_world_inner";
        lock_guard<mutex> lockConfiguration(_configurationMutex);

        // Process tree updates
        Timer::get() << "tree_process";
        _tree.processQueue(true);
        Timer::get() >> "tree_process";

        // Execute waiting tasks
        executeTreeCommands();
        runTasks();

        {
            lock_guard<recursive_mutex> lockObjects(_objectsMutex);

            // Read and serialize new buffers
            Timer::get() << "serialize";
            unordered_map<string, shared_ptr<SerializedObject>> serializedObjects;
            {
                vector<future<void>> threads;
                for (auto& o : _objects)
                {
                    // Run object tasks
                    o.second->runTasks();

                    auto bufferObj = dynamic_pointer_cast<BufferObject>(o.second);
                    // This prevents the map structure to be modified in the threads
                    auto serializedObjectIt = serializedObjects.emplace(std::make_pair(bufferObj->getDistantName(), shared_ptr<SerializedObject>(nullptr)));
                    if (!serializedObjectIt.second)
                        continue; // Error while inserting the object in the map

                    threads.push_back(async(launch::async, [=]() {
                        // Update the local objects
                        o.second->update();

                        // Send them the their destinations
                        if (bufferObj.get() != nullptr)
                        {
                            if (bufferObj->wasUpdated()) // if the buffer has been updated
                            {
                                auto obj = bufferObj->serialize();
                                bufferObj->setNotUpdated();
                                if (obj)
                                    serializedObjectIt.first->second = obj;
                            }
                        }
                    }));
                }
            }
            Timer::get() >> "serialize";

            // Wait for previous buffers to be uploaded
            _link->waitForBufferSending(chrono::milliseconds(50)); // Maximum time to wait for frames to arrive
            sendMessage(SPLASH_ALL_PEERS, "uploadTextures", {});
            Timer::get() >> "upload";

            // Ask for the upload of the new buffers, during the next world loop
            Timer::get() << "upload";
            for (auto& o : serializedObjects)
                if (o.second)
                    _link->sendBuffer(o.first, std::move(o.second));
        }

        if (_quit)
        {
            for (auto& s : _scenes)
                sendMessage(s.first, "quit", {});
            break;
        }

        Timer::get() << "tree_propagate";
        updateTreeFromObjects();
        propagateTree();
        Timer::get() >> "tree_propagate";

        // Sync with buffer object update
        Timer::get() >> "loop_world_inner";
        auto elapsed = Timer::get().getDuration("loop_world_inner");
        waitSignalBufferObjectUpdated(std::max<uint64_t>(1, 1e6 / (float)_worldFramerate - elapsed));

        // Sync to world framerate
        Timer::get() >> "loop_world";
    }
}

/*************/
void World::addToWorld(const string& type, const string& name)
{
    // BufferObject derived types have a counterpart on this side
    if (!_factory->isSubtype<BufferObject>(type))
        return;

    auto object = _factory->create(type);
    auto realName = name;
    if (object)
    {
        object->setName(name);
        _objects[name] = object;
    }
}

/*************/
void World::applyConfig()
{
    lock_guard<mutex> lockConfiguration(_configurationMutex);

    // We first destroy all scene and objects
    _scenes.clear();
    _objects.clear();
    _masterSceneName = "";

    try
    {
        // Get the list of all scenes, and create them
        if (!_config.isMember("scenes"))
        {
            Log::get() << Log::ERROR << "World::" << __FUNCTION__ << " - Error while getting scenes configuration" << Log::endl;
            return;
        }

        const Json::Value& scenes = _config["scenes"];
        for (const auto& sceneName : scenes.getMemberNames())
        {
            string sceneAddress = scenes[sceneName].isMember("address") ? scenes[sceneName]["address"].asString() : "localhost";
            string sceneDisplay = scenes[sceneName].isMember("display") ? scenes[sceneName]["display"].asString() : "";
            bool spawn = scenes[sceneName].isMember("spawn") ? scenes[sceneName]["spawn"].asBool() : true;

            if (!addScene(sceneName, sceneDisplay, sceneAddress, spawn && _spawnSubprocesses))
                continue;

            // Set the remaining parameters
            for (const auto& paramName : scenes[sceneName].getMemberNames())
            {
                auto values = jsonToValues(scenes[sceneName][paramName]);
                sendMessage(sceneName, paramName, values);
            }
        }

        // Reseeds the world branch into the Scene's trees
        propagatePath("/world");

        // Configure each scenes
        // The first scene is the master one, and also receives some ghost objects
        // First, set the master scene
        sendMessage(_masterSceneName, "setMaster", {_configFilename});

        // Then, we create the objects
        for (const auto& scene : _scenes)
        {
            const Json::Value& objects = _config["scenes"][scene.first]["objects"];
            if (!objects)
                continue;

            // Create the objects
            auto sceneMembers = objects.getMemberNames();
            for (const auto& objectName : objects.getMemberNames())
            {
                if (!objects[objectName].isMember("type"))
                    continue;

                setAttribute("addObject", {objects[objectName]["type"].asString(), objectName, scene.first, false});
            }

            sendMessage(SPLASH_ALL_PEERS, "runInBackground", {_runInBackground});
        }

        // Make sure all objects have been created in every Scene, by sending a sync message
        for (const auto& s : _scenes)
            sendMessageWithAnswer(s.first, "sync");

        // Then we link the objects together
        for (auto& s : _scenes)
        {
            const Json::Value& scene = _config["scenes"][s.first];
            auto sceneMembers = scene.getMemberNames();
            const Json::Value& links = scene["links"];
            if (!links)
                continue;

            for (auto& link : links)
            {
                if (link.size() < 2)
                    continue;
                addTask([=]() { sendMessage(SPLASH_ALL_PEERS, "link", {link[0].asString(), link[1].asString()}); });
            }
        }

        // Configure the objects
        for (auto& s : _scenes)
        {
            const Json::Value& objects = _config["scenes"][s.first]["objects"];
            if (!objects)
                continue;

            // Create the objects
            for (const auto& objectName : objects.getMemberNames())
            {
                auto& obj = objects[objectName];

                addTask([=]() {
                    // Set their attributes
                    auto objMembers = obj.getMemberNames();
                    int idxAttr = 0;
                    for (const auto& attr : obj)
                    {
                        if (objMembers[idxAttr] == "type")
                        {
                            idxAttr++;
                            continue;
                        }

                        auto values = jsonToValues(attr);
                        values.push_front(objMembers[idxAttr]);
                        values.push_front(objectName);
                        setAttribute("sendAll", values);

                        idxAttr++;
                    }
                });
            }
        }

        // Lastly, configure this very World
        // This happens last as some parameters are sent to Scenes (like blending computation)
        if (_config.isMember("world"))
        {
            const Json::Value jsWorld = _config["world"];
            auto worldMember = jsWorld.getMemberNames();
            int idx{0};
            for (const auto& attr : jsWorld)
            {
                auto values = jsonToValues(attr);
                string paramName = worldMember[idx];
                setAttribute(paramName, values);
                idx++;
            }
        }
    }
    catch (...)
    {
        Log::get() << Log::ERROR << "Exception caught while applying configuration from file " << _configFilename << Log::endl;
        return;
    }

// Also, enable the master clock if it was not enabled
#if HAVE_PORTAUDIO
    addTask([&]() {
        if (!_clock)
            _clock = unique_ptr<LtcClock>(new LtcClock(true));
    });
#endif

    // Send the start message for all scenes
    for (auto& s : _scenes)
    {
        auto answer = sendMessageWithAnswer(s.first, "start", {}, 2e6);
        if (0 == answer.size())
        {
            Log::get() << Log::ERROR << "World::" << __FUNCTION__ << " - Timeout when trying to connect to scene \"" << s.first << "\". Exiting." << Log::endl;
            _quit = true;
            break;
        }
    }
}

/*************/
bool World::addScene(const std::string& sceneName, const std::string& sceneDisplay, const std::string& sceneAddress, bool spawn)
{
    if (sceneAddress == "localhost")
    {
        string display{""};
        string worldDisplay{"none"};
#if HAVE_LINUX
        auto regDisplayFull = regex("(:[0-9]\\.[0-9])", regex_constants::extended);
        auto regDisplayInt = regex("[0-9]", regex_constants::extended);
        smatch match;

        if (getenv("DISPLAY") != nullptr)
        {
            worldDisplay = getenv("DISPLAY");
            if (!worldDisplay.empty() && worldDisplay.find(".") == string::npos)
                worldDisplay += ".0";
        }

        display = "DISPLAY=" + worldDisplay;
        if (!sceneDisplay.empty())
        {
            if (regex_match(sceneDisplay, match, regDisplayFull))
                display = "DISPLAY=" + sceneDisplay;
            else if (regex_match(sceneDisplay, match, regDisplayInt))
                display = "DISPLAY=:" + _displayServer + "." + sceneDisplay;
        }

        if (!_forcedDisplay.empty())
        {
            if (regex_match(_forcedDisplay, match, regDisplayFull))
                display = "DISPLAY=" + _forcedDisplay;
            else if (regex_match(_forcedDisplay, match, regDisplayInt))
                display = "DISPLAY=:" + _displayServer + "." + _forcedDisplay;
        }
#endif

        int pid = -1;
        if (spawn)
        {
            _sceneLaunched = false;

            // If the current process is on the correct display, we use an inner Scene
            if (worldDisplay.size() > 0 && display.find(worldDisplay) == display.size() - worldDisplay.size() && !_innerScene)
            {
                Log::get() << Log::MESSAGE << "World::" << __FUNCTION__ << " - Starting an inner Scene" << Log::endl;
                _innerScene = make_shared<Scene>(sceneName, _linkSocketPrefix);
                _innerSceneThread = thread([&]() { _innerScene->run(); });
            }
            else
            {
                // Spawn a new process containing this Scene
                Log::get() << Log::MESSAGE << "World::" << __FUNCTION__ << " - Starting a Scene in another process" << Log::endl;

                string cmd = _currentExePath;
                string debug = (Log::get().getVerbosity() == Log::DEBUGGING) ? "-d" : "";
                string timer = Timer::get().isDebug() ? "-t" : "";
                string slave = "--child";
                string xauth = "XAUTHORITY=" + Utils::getHomePath() + "/.Xauthority";

                vector<char*> argv = {const_cast<char*>(cmd.c_str()), const_cast<char*>(slave.c_str())};
                if (!_linkSocketPrefix.empty())
                {
                    argv.push_back((char*)"--prefix");
                    argv.push_back(const_cast<char*>(_linkSocketPrefix.c_str()));
                }
                if (!debug.empty())
                    argv.push_back(const_cast<char*>(debug.c_str()));
                if (!timer.empty())
                    argv.push_back(const_cast<char*>(timer.c_str()));
                argv.push_back(const_cast<char*>(sceneName.c_str()));
                argv.push_back(nullptr);
                vector<char*> env = {const_cast<char*>(display.c_str()), const_cast<char*>(xauth.c_str()), nullptr};

                int status = posix_spawn(&pid, cmd.c_str(), nullptr, nullptr, argv.data(), env.data());
                if (status != 0)
                    Log::get() << Log::ERROR << "World::" << __FUNCTION__ << " - Error while spawning process for scene " << sceneName << Log::endl;
            }

            // We wait for the child process to be launched
            unique_lock<mutex> lockChildProcess(_childProcessMutex);
            while (!_sceneLaunched)
            {
                if (cv_status::timeout == _childProcessConditionVariable.wait_for(lockChildProcess, chrono::seconds(5)))
                {
                    Log::get() << Log::ERROR << "World::" << __FUNCTION__ << " - Timeout when trying to connect to newly spawned scene \"" << sceneName << "\". Exiting."
                               << Log::endl;
                    _quit = true;
                    return false;
                }
            }
        }

        _scenes[sceneName] = pid;
        if (_masterSceneName.empty())
            _masterSceneName = sceneName;

        // Initialize the communication
        if (pid == -1 && spawn)
            _link->connectTo(sceneName, _innerScene.get());
        else
            _link->connectTo(sceneName);

        return true;
    }
    else
    {
        Log::get() << Log::WARNING << "World::" << __FUNCTION__ << " - Non-local scenes are not implemented yet" << Log::endl;
        return false;
    }
}

/*************/
string World::getObjectsAttributesDescriptions()
{
    Json::Value root;

    auto formatDescription = [](const string desc, const Values& argTypes) -> string {
        string descriptionStr = "[";
        for (uint32_t i = 0; i < argTypes.size(); ++i)
        {
            descriptionStr += argTypes[i].as<string>();
            if (i < argTypes.size() - 1)
                descriptionStr += ", ";
        }
        descriptionStr += "] " + desc;

        return descriptionStr;
    };

    // We create "fake" objects and ask then for their attributes
    auto localFactory = Factory();
    auto types = localFactory.getObjectTypes();
    for (auto& type : types)
    {
        auto obj = localFactory.create(type);
        if (!obj)
            continue;

        root[obj->getType() + "_short_description"] = localFactory.getShortDescription(type);
        root[obj->getType() + "_description"] = localFactory.getDescription(type);

        auto attributesDescriptions = obj->getAttributesDescriptions();
        int addedAttribute = 0;
        root[obj->getType()] = Json::Value();
        for (auto& d : attributesDescriptions)
        {
            // We only keep attributes with a valid documentation
            // The other ones are inner attributes
            if (d[1].as<string>().size() == 0)
                continue;

            // We also don't keep attributes with no argument types
            if (d[2].as<Values>().size() == 0)
                continue;

            root[obj->getType()][d[0].as<string>()] = formatDescription(d[1].as<string>(), d[2].as<Values>());

            addedAttribute++;
        }

        // If the object has no documented attribute
        if (addedAttribute == 0)
            root.removeMember(obj->getType());
    }

    // Also, add documentation for the World and Scene types
    auto worldDescription = getAttributesDescriptions();
    for (auto& d : worldDescription)
    {
        if (d[1].size() == 0)
            continue;

        root["world"][d[0].as<string>()] = formatDescription(d[1].as<string>(), d[2].as<Values>());
    }

    setlocale(LC_NUMERIC, "C"); // Needed to make sure numbers are written with commas
    string jsonString;
    jsonString = root.toStyledString();

    return jsonString;
}

/*************/
void World::saveConfig()
{
    setlocale(LC_NUMERIC, "C"); // Needed to make sure numbers are written with commas

    // Local objects configuration can differ from the scenes objects,
    // as their type is not necessarily identical
    for (const auto& sceneName : _config["scenes"].getMemberNames())
    {
        if (!_tree.hasBranchAt("/" + sceneName))
            continue;

        // Set the scene configuration from what was received in the previous loop
        auto scene = getRootConfigurationAsJson(sceneName);
        for (const auto& attr : scene.getMemberNames())
        {
            if (attr != "objects")
            {
                _config["scenes"][sceneName][attr] = scene[attr];
            }
            else
            {
                Json::Value::Members objectNames = scene["objects"].getMemberNames();

                _config["scenes"][sceneName][attr] = Json::Value();
                Json::Value& objects = _config["scenes"][sceneName]["objects"];

                for (auto& m : objectNames)
                    for (const auto& a : scene["objects"][m].getMemberNames())
                        objects[m][a] = scene["objects"][m][a];
            }
        }
    }

    // Configuration from the world
    _config["description"] = SPLASH_FILE_CONFIGURATION;
    _config["version"] = string(PACKAGE_VERSION);
    auto worldConfiguration = getRootConfigurationAsJson("world");
    for (const auto& attr : worldConfiguration.getMemberNames())
    {
        _config["world"][attr] = worldConfiguration[attr];
    }

    setlocale(LC_NUMERIC, "C"); // Needed to make sure numbers are written with commas
    ofstream out(_configFilename, ios::binary);
    out << _config.toStyledString();
    out.close();
}

/*************/
void World::saveProject()
{
    try
    {
        setlocale(LC_NUMERIC, "C"); // Needed to make sure numbers are written with commas

        auto root = Json::Value(); // Haha, auto root...
        root["description"] = SPLASH_FILE_PROJECT;
        root["version"] = string(PACKAGE_VERSION);
        root["links"] = Json::Value();

        // Here, we don't care about which Scene holds which object, as objects with the
        // same name in different Scenes are necessarily clones
        std::set<std::pair<string, string>> existingLinks{}; // We keep a list of already existing links
        for (auto& s : _scenes)
        {
            auto config = getRootConfigurationAsJson(s.first);

            for (auto& v : config["links"])
            {
                // Only keep links to partially saved types
                bool isSavableType = _factory->isProjectSavable(config["objects"][v[0].asString()]["type"].asString());
                // If the object is linked to a camera, we save the link as
                // "saved to all available cameras"
                bool isLinkedToCam = (config["objects"][v[1].asString()]["type"].asString() == "camera");

                if (isLinkedToCam)
                    v[1] = SPLASH_CAMERA_LINK;

                auto link = make_pair<string, string>(v[0].asString(), v[1].asString());
                if (existingLinks.find(link) == existingLinks.end())
                    existingLinks.insert(link);
                else
                    continue;

                if (isSavableType)
                    root["links"].append(v);
            }

            for (const auto& member : config["objects"].getMemberNames())
            {
                // We only save configuration for non Scene-specific objects, which are one of the following:
                if (!_factory->isProjectSavable(config["objects"][member]["type"].asString()))
                    continue;

                for (const auto& attr : config["objects"][member].getMemberNames())
                    root["objects"][member][attr] = config["objects"][member][attr];
            }
        }

        ofstream out(_projectFilename, ios::binary);
        out << root.toStyledString();
        out.close();
    }
    catch (...)
    {
        Log::get() << Log::ERROR << "Exception caught while saving file " << _projectFilename << Log::endl;
    }
}

/*************/
vector<string> World::getObjectsOfType(const string& type) const
{
    vector<string> objectList;

    for (const auto& rootName : _tree.getBranchList())
    {
        auto objectsPath = "/" + rootName + "/objects";
        for (const auto& objectName : _tree.getBranchListAt(objectsPath))
        {
            if (type.empty())
                objectList.push_back(objectName);

            auto typePath = objectsPath + "/" + objectName + "/type";
            assert(_tree.hasLeafAt(typePath));
            Value typeValue;
            _tree.getValueForLeafAt(typePath, typeValue);
            if (typeValue[0].as<string>() == type)
                objectList.push_back(objectName);
        }
    }

    std::sort(objectList.begin(), objectList.end());
    objectList.erase(std::unique(objectList.begin(), objectList.end()), objectList.end());

    return objectList;
}

/*************/
bool World::handleSerializedObject(const string& name, shared_ptr<SerializedObject> obj)
{
    if (!RootObject::handleSerializedObject(name, obj))
        _link->sendBuffer(name, obj);
    return true;
}

/*************/
void World::init()
{
    // If set to run as a child process, we do not initialize anything
    if (!_runAsChild)
    {
        _name = "world";

        _that = this;
        _signals.sa_handler = leave;
        _signals.sa_flags = 0;
        sigaction(SIGINT, &_signals, nullptr);
        sigaction(SIGTERM, &_signals, nullptr);

        if (_linkSocketPrefix.empty())
            _linkSocketPrefix = to_string(static_cast<int>(getpid()));
        _link = make_unique<Link>(this, _name);

        registerAttributes();
        initializeTree();
    }
}

/*************/
void World::leave(int /*signal_value*/)
{
    Log::get() << "World::" << __FUNCTION__ << " - Received a SIG event. Quitting." << Log::endl;
    _that->_quit = true;
}

/*************/
bool World::copyCameraParameters(const std::string& filename)
{
    // List of copyable types
    static vector<string> copyableTypes{"camera", "warp"};

    Json::Value config;
    if (!Utils::loadJsonFile(filename, config))
        return false;

    // Get the scene names from this other configuration file
    for (const auto& s : config["scenes"].getMemberNames())
    {
        // Look for the cameras in the configuration file
        for (const auto& name : config["scenes"][s]["objects"].getMemberNames())
        {
            Json::Value& obj = config["scenes"][s]["objects"][name];
            if (find(copyableTypes.begin(), copyableTypes.end(), obj["type"].asString()) == copyableTypes.end())
                continue;

            // Go through the camera attributes
            for (const auto& attrName : obj.getMemberNames())
            {
                Json::Value& attr = obj[attrName];
                if (attrName == "type")
                    continue;

                auto values = jsonToValues(attr);

                // Send the new values for this attribute
                _tree.setValueForLeafAt("/" + s + "/objects/" + name + "/attributes/" + attrName, values);
            }
        }
    }

    return true;
}

/*************/
Values World::jsonToValues(const Json::Value& values)
{
    Values outValues;

    if (values.isInt())
        outValues.emplace_back(values.asInt());
    else if (values.isDouble())
        outValues.emplace_back(values.asFloat());
    else if (values.isArray())
    {
        for (const auto& v : values)
        {
            if (v.isInt())
                outValues.emplace_back(v.asInt());
            else if (v.isDouble())
                outValues.emplace_back(v.asFloat());
            else if (v.isArray() || v.isObject())
                outValues.emplace_back(jsonToValues(v));
            else
                outValues.emplace_back(v.asString());
        }
    }
    else if (values.isObject())
    {
        auto names = values.getMemberNames();
        int index = 0;
        for (const auto& v : values)
        {
            if (v.isInt())
                outValues.emplace_back(v.asInt(), names[index]);
            else if (v.isDouble())
                outValues.emplace_back(v.asFloat(), names[index]);
            else if (v.isArray() || v.isObject())
                outValues.emplace_back(jsonToValues(v), names[index]);
            else
            {
                outValues.emplace_back(v.asString());
                outValues.back().setName(names[index]);
            }

            ++index;
        }
    }
    else
        outValues.emplace_back(values.asString());

    return outValues;
}

/*************/
bool World::loadConfig(const string& filename, Json::Value& configuration)
{
    if (!Utils::loadJsonFile(filename, configuration))
        return false;

    if (!Utils::checkAndUpgradeConfiguration(configuration))
        return false;

    _configFilename = filename;
    _configurationPath = Utils::getPathFromFilePath(_configFilename);
    _mediaPath = _configurationPath; // By default, same directory
    return true;
}

/*************/
bool World::loadProject(const string& filename)
{
    try
    {
        Json::Value partialConfig;
        if (!Utils::loadJsonFile(filename, partialConfig))
            return false;

        if (!partialConfig.isMember("description") || partialConfig["description"].asString() != SPLASH_FILE_PROJECT)
            return false;

        _projectFilename = filename;
        // The configuration path is overriden with the project file path
        _configurationPath = Utils::getPathFromFilePath(_projectFilename);

        // Now, we apply the configuration depending on the current state
        // Meaning, we replace objects with the same name, create objects with non-existing name,
        // and delete objects which are not in the partial config

        // Delete existing objects
        for (const auto& s : _scenes)
        {
            const Json::Value& sceneObjects = _config["scenes"][s.first]["objects"];
            for (const auto& member : sceneObjects.getMemberNames())
            {
                if (!sceneObjects[member].isMember("type"))
                    continue;
                if (_factory->isProjectSavable(sceneObjects[member]["type"].asString()))
                    setAttribute("deleteObject", {member});
            }
        }

        // Create new objects
        for (const auto& objectName : partialConfig["objects"].getMemberNames())
        {
            if (!partialConfig["objects"][objectName].isMember("type"))
                continue;
            setAttribute("addObject", {partialConfig["objects"][objectName]["type"].asString(), objectName, "", false});
        }

        // Handle the links
        // We will need a list of all cameras
        for (const auto& link : partialConfig["links"])
        {
            if (link.size() != 2)
                continue;
            auto source = link[0].asString();
            auto sink = link[1].asString();

            addTask([=]() {
                if (sink != SPLASH_CAMERA_LINK)
                {
                    sendMessage(SPLASH_ALL_PEERS, "link", {link[0].asString(), link[1].asString()});
                }
                else
                {
                    auto cameraNames = getObjectsOfType("camera");
                    for (const auto& camera : cameraNames)
                        sendMessage(SPLASH_ALL_PEERS, "link", {link[0].asString(), camera});
                }
            });
        }

        // Configure the objects
        for (const auto& objectName : partialConfig["objects"].getMemberNames())
        {
            auto& obj = partialConfig["objects"][objectName];
            auto configPath = Utils::getPathFromFilePath(_configFilename);

            addTask([=]() {
                // Set their attributes
                auto objMembers = obj.getMemberNames();
                int idxAttr = 0;
                for (const auto& attr : obj)
                {
                    if (objMembers[idxAttr] == "type")
                    {
                        idxAttr++;
                        continue;
                    }

                    auto values = jsonToValues(attr);
                    values.push_front(objMembers[idxAttr]);
                    values.push_front(objectName);
                    setAttribute("sendAll", values);

                    idxAttr++;
                }
            });
        }

        return true;
    }
    catch (...)
    {
        Log::get() << Log::ERROR << "Exception caught while loading file " << filename << Log::endl;
        return false;
    }
}

/*************/
void World::parseArguments(int argc, char** argv)
{
    auto printWelcome = []() {
        cout << endl;
        cout << "\t             \033[33;1m- Splash -\033[0m" << endl;
        cout << "\t\033[1m- Modular multi-output video mapper -\033[0m" << endl;
        cout << "\t          \033[1m- Version " << PACKAGE_VERSION << " -\033[0m" << endl;
        cout << endl;
    };

    // Get the executable directory
    _splashExecutable = argv[0];
    _currentExePath = Utils::getCurrentExecutablePath();
    _executionPath = Utils::getPathFromExecutablePath(_splashExecutable);

    // Parse the other args
    string filename = string(DATADIR) + "splash.json";
    bool defaultFile = true;

    while (true)
    {
        static struct option longOptions[] = {
            {"debug", no_argument, 0, 'd'},
#if HAVE_LINUX
            {"forceDisplay", required_argument, 0, 'D'},
            {"displayServer", required_argument, 0, 'S'},
#endif
            {"help", no_argument, 0, 'h'},
            {"hide", no_argument, 0, 'H'},
            {"info", no_argument, 0, 'i'},
            {"log2file", no_argument, 0, 'l'},
            {"open", required_argument, 0, 'o'},
            {"prefix", required_argument, 0, 'p'},
            {"silent", no_argument, 0, 's'},
            {"timer", no_argument, 0, 't'},
            {"child", no_argument, 0, 'c'},
            {"spawnProcesses", required_argument, 0, 'x'},
            {0, 0, 0, 0}
        };

        int optionIndex = 0;
        auto ret = getopt_long(argc, argv, "+cdD:S:hHilo:p:P:stx", longOptions, &optionIndex);

        if (ret == -1)
            break;

        switch (ret)
        {
        default:
        case 'h':
        {
            printWelcome();

            cout << "Basic usage: splash [arguments] [config.json] -- [python script argument]" << endl;
            cout << "Options:" << endl;
            cout << "\t-o (--open) [filename] : set [filename] as the configuration file to open" << endl;
            cout << "\t-d (--debug) : activate debug messages (if Splash was compiled with -DDEBUG)" << endl;
            cout << "\t-t (--timer) : activate more timers, at the cost of performance" << endl;
#if HAVE_LINUX
            cout << "\t-D (--forceDisplay) : force the display on which to show all windows" << endl;
            cout << "\t-S (--displayServer) : set the display server ID" << endl;
#endif
            cout << "\t-s (--silent) : disable all messages" << endl;
            cout << "\t-i (--info) : get description for all objects attributes" << endl;
            cout << "\t-H (--hide) : run Splash in background" << endl;
            cout << "\t-P (--python) : add the given Python script to the loaded configuration" << endl;
            cout << "                  any argument after -- will be sent to the script" << endl;
            cout << "\t-l (--log2file) : write the logs to /var/log/splash.log, if possible" << endl;
            cout << "\t-p (--prefix) : set the shared memory socket paths prefix (defaults to the PID)" << endl;
            cout << "\t-c (--child): run as a child controlled by a master Splash process" << endl;
            cout << "\t-x (--doNotSpawn): do not spawn subprocesses, which have to be ran manually" << endl;
            cout << endl;
            exit(0);
        }
        case 'd':
        {
            Log::get().setVerbosity(Log::DEBUGGING);
            break;
        }
        case 'D':
        {
            auto regDisplayFull = regex("(:[0-9]\\.[0-9])", regex_constants::extended);
            auto regDisplayInt = regex("[0-9]", regex_constants::extended);
            smatch match;

            _forcedDisplay = string(optarg);
            if (regex_match(_forcedDisplay, match, regDisplayFull))
            {
                Log::get() << Log::MESSAGE << "World::" << __FUNCTION__ << " - Display forced to " << _forcedDisplay << Log::endl;
            }
            else if (regex_match(_forcedDisplay, match, regDisplayInt))
            {
                Log::get() << Log::MESSAGE << "World::" << __FUNCTION__ << " - Display forced to :0." << _forcedDisplay << Log::endl;
            }
            else
            {
                Log::get() << Log::WARNING << "World::" << __FUNCTION__ << " - " << string(optarg) << ": argument expects a positive integer, or a string in the form of \":x.y\""
                           << Log::endl;
                exit(0);
            }
            break;
        }
        case 'S':
        {
            auto regInt = regex("[0-9]+", regex_constants::extended);
            smatch match;

            _displayServer = string(optarg);
            if (regex_match(_displayServer, match, regInt))
            {
                Log::get() << Log::MESSAGE << "World::" << __FUNCTION__ << " - Display server forced to :" << _displayServer << Log::endl;
            }
            else
            {
                Log::get() << Log::WARNING << "World::" << __FUNCTION__ << " - " << string(optarg) << ": argument expects a positive integer" << Log::endl;
                exit(0);
            }
            break;
        }
        case 'H':
        {
            _runInBackground = true;
            break;
        }
        case 'P':
        {
            auto pythonScriptPath = Utils::getFullPathFromFilePath(string(optarg), Utils::getCurrentWorkingDirectory());

            // Build the Python arg list
            auto pythonArgs = Values({pythonScriptPath});
            bool isPythonArg = false;
            for (int i = 0; i < argc; ++i)
            {
                if (!isPythonArg && "--" == string(argv[i]))
                {
                    isPythonArg = true;
                    continue;
                }
                else if (!isPythonArg)
                {
                    continue;
                }
                else
                {
                    pythonArgs.push_back(string(argv[i]));
                }
            }

            // The Python script will be added once the loop runs
            addTask([=]() {
                Log::get() << Log::MESSAGE << "World::parseArguments - Adding Python script from command line argument: " << pythonScriptPath << Log::endl;
                auto pythonObjectName = string("_pythonArgScript");
                if (!_nameRegistry.registerName(pythonObjectName))
                    pythonObjectName = _nameRegistry.generateName("_pythonArgScript");
                sendMessage(SPLASH_ALL_PEERS, "addObject", {"python", pythonObjectName, _masterSceneName});
                sendMessage(pythonObjectName, "savable", {false});
                sendMessage(pythonObjectName, "args", {pythonArgs});
                sendMessage(pythonObjectName, "file", {pythonScriptPath});
            });
            break;
        }
        case 'i':
        {
            auto descriptions = getObjectsAttributesDescriptions();
            cout << descriptions << endl;
            exit(0);
        }
        case 'l':
        {
            setAttribute("logToFile", {1});
            addTask([&]() { setAttribute("logToFile", {1}); });
            break;
        }
        case 'o':
        {
            defaultFile = false;
            filename = string(optarg);
            break;
        }
        case 'p':
        {
            _linkSocketPrefix = string(optarg);
            break;
        }
        case 's':
        {
            Log::get().setVerbosity(Log::NONE);
            break;
        }
        case 't':
        {
            Timer::get().setDebug(true);
            break;
        }
        case 'c':
        {
            _runAsChild = true;
            break;
        }
        case 'x':
        {
            _spawnSubprocesses = false;
            break;
        }
        }
    }

    // Find last argument index, or "--"
    int lastArgIndex = 0;
    for (; lastArgIndex < argc; ++lastArgIndex)
        if (string(argv[lastArgIndex]) == "--")
            break;

    string lastArg = "";
    if (optind < lastArgIndex)
        lastArg = string(argv[optind]);

    if (_runAsChild)
    {
        if (!lastArg.empty())
            _childSceneName = lastArg;
    }
    else
    {
        printWelcome();

        if (!lastArg.empty())
        {
            filename = lastArg;
            defaultFile = false;
        }
        if (filename != "")
        {
            Json::Value config;
            _status &= loadConfig(filename, config);

            if (_status)
                _config = config;
            else
                exit(0);
        }
        else
        {
            exit(0);
        }
    }

    if (defaultFile && !_runAsChild)
        Log::get() << Log::MESSAGE << "No filename specified, loading default file" << Log::endl;
    else if (!_runAsChild)
        Log::get() << Log::MESSAGE << "Loading file " << filename << Log::endl;
}

/*************/
void World::registerAttributes()
{
    addAttribute("addObject",
        [&](const Values& args) {
            addTask([=]() {
                auto type = args[0].as<string>();
                auto name = args.size() < 2 ? "" : args[1].as<string>();
                auto scene = args.size() < 3 ? "" : args[2].as<string>();
                auto checkName = args.size() < 4 ? true : args[3].as<bool>();

                lock_guard<recursive_mutex> lockObjects(_objectsMutex);

                if (checkName && (name.empty() || !_nameRegistry.registerName(name)))
                    name = _nameRegistry.generateName(type);

                if (scene.empty())
                {
                    addToWorld(type, name);
                    for (auto& s : _scenes)
                    {
                        sendMessage(s.first, "addObject", {type, name, s.first});
                        sendMessageWithAnswer(s.first, "sync");
                    }
                }
                else
                {
                    addToWorld(type, name);
                    sendMessage(scene, "addObject", {type, name, scene});
                    if (scene != _masterSceneName)
                        sendMessage(_masterSceneName, "addObject", {type, name, scene});
                    sendMessageWithAnswer(scene, "sync");
                }

                set(name, "configFilePath", {Utils::getPathFromFilePath(_configFilename)}, false);
            });

            return true;
        },
        {'s'});
    setAttributeDescription("addObject", "Add an object to the scenes");

    addAttribute("sceneLaunched", [&](const Values&) {
        lock_guard<mutex> lockChildProcess(_childProcessMutex);
        _sceneLaunched = true;
        _childProcessConditionVariable.notify_all();
        return true;
    });
    setAttributeDescription("sceneLaunched", "Message sent by Scenes to confirm they are running");

    addAttribute("deleteObject",
        [&](const Values& args) {
            addTask([=]() {
                lock_guard<recursive_mutex> lockObjects(_objectsMutex);
                auto objectName = args[0].as<string>();

                // Delete the object here
                _nameRegistry.unregisterName(objectName);
                auto objectIt = _objects.find(objectName);
                if (objectIt != _objects.end())
                    _objects.erase(objectIt);

                // Ask for Scenes to delete the object
                sendMessage(SPLASH_ALL_PEERS, "deleteObject", args);

                for (const auto& s : _scenes)
                    sendMessageWithAnswer(s.first, "sync");
            });

            return true;
        },
        {'s'});
    setAttributeDescription("deleteObject", "Delete an object given its name");

    addAttribute("link",
        [&](const Values& args) {
            addTask([=]() { sendMessage(SPLASH_ALL_PEERS, "link", args); });
            return true;
        },
        {'s', 's'});
    setAttributeDescription("link", "Link the two given objects");

    addAttribute("unlink",
        [&](const Values& args) {
            addTask([=]() { sendMessage(SPLASH_ALL_PEERS, "unlink", args); });
            return true;
        },
        {'s', 's'});
    setAttributeDescription("unlink", "Unlink the two given objects");

    addAttribute("loadConfig",
        [&](const Values& args) {
            string filename = args[0].as<string>();
            runAsyncTask([=]() {
                Json::Value config;
                if (loadConfig(filename, config))
                {
                    for (auto& s : _scenes)
                    {
                        sendMessage(s.first, "quit", {});
                        _link->disconnectFrom(s.first);
                        if (s.second != -1)
                        {
                            waitpid(s.second, nullptr, 0);
                        }
                        else
                        {
                            if (_innerSceneThread.joinable())
                                _innerSceneThread.join();
                            _innerScene.reset();
                        }
                    }

                    _masterSceneName = "";

                    _config = config;
                    applyConfig();
                }
            });
            return true;
        },
        {'s'});
    setAttributeDescription("loadConfig", "Load the given configuration file");

    addAttribute("copyCameraParameters",
        [&](const Values& args) {
            string filename = args[0].as<string>();
            addTask([=]() { copyCameraParameters(filename); });
            return true;
        },
        {'s'});
    setAttributeDescription("copyCameraParameters", "Copy the camera parameters from the given configuration file (based on camera names)");

    addAttribute("pong",
        [&](const Values& args) {
            Timer::get() >> ("pingScene " + args[0].as<string>());
            return true;
        },
        {'s'});

    addAttribute("quit", [&](const Values&) {
        _quit = true;
        return true;
    });
    setAttributeDescription("quit", "Ask the world to quit");

    addAttribute("replaceObject",
        [&](const Values& args) {
            auto objName = args[0].as<string>();
            auto objType = args[1].as<string>();
            auto objAlias = args[2].as<string>();
            vector<string> targets;
            for (uint32_t i = 3; i < args.size(); ++i)
                targets.push_back(args[i].as<string>());

            if (!_factory->isCreatable(objType))
                return false;

            setAttribute("deleteObject", {objName});
            setAttribute("addObject", {objType, objName, "", false});
            addTask([=]() {
                for (const auto& t : targets)
                    setAttribute("sendAllScenes", {"link", objName, t});
            });
            return true;
        },
        {'s', 's'});
    setAttributeDescription("replaceObject",
        "Replace the given object by an object of the given type, with the given alias, and links the new object to the objects given by the following parameters");

    addAttribute("save", [&](const Values& args) {
        if (args.size() != 0)
            _configFilename = args[0].as<string>();

        addTask([=]() {
            Log::get() << "Saving configuration" << Log::endl;
            saveConfig();
        });
        return true;
    });
    setAttributeDescription("save", "Save the configuration to the current file (or a new one if a name is given as parameter)");

    addAttribute("saveProject",
        [&](const Values& args) {
            _projectFilename = args[0].as<string>();
            addTask([=]() {
                Log::get() << "Saving partial configuration to " << _projectFilename << Log::endl;
                saveProject();
            });
            return true;
        },
        {'s'});
    setAttributeDescription("saveProject", "Save only the configuration of images, textures and meshes");

    addAttribute("loadProject",
        [&](const Values& args) {
            _projectFilename = args[0].as<string>();
            addTask([=]() {
                Log::get() << "Loading partial configuration from " << _projectFilename << Log::endl;
                loadProject(_projectFilename);
            });
            return true;
        },
        {'s'});
    setAttributeDescription("loadProject", "Load only the configuration of images, textures and meshes");

    addAttribute("logToFile",
        [&](const Values& args) {
            Log::get().logToFile(args[0].as<bool>());
            setAttribute("sendAllScenes", {"logToFile", args[0]});
            return true;
        },
        {'n'});
    setAttributeDescription("logToFile", "If set to 1, the process holding the World will try to write log to file");

    addAttribute("sendAll",
        [&](const Values& args) {
            addTask([=]() {
                string name = args[0].as<string>();
                string attr = args[1].as<string>();
                auto values = args;

                // Send the updated values to all scenes
                values.erase(values.begin());
                values.erase(values.begin());
                sendMessage(name, attr, values);

                // Also update local version
                if (_objects.find(name) != _objects.end())
                    _objects[name]->setAttribute(attr, values);
            });

            return true;
        },
        {'s', 's'});
    setAttributeDescription("sendAll", "Send to the given object in all Scenes the given message (all following arguments)");

    addAttribute("sendAllScenes",
        [&](const Values& args) {
            string attr = args[0].as<string>();
            Values values = args;
            values.erase(values.begin());
            for (auto& scene : _scenes)
                sendMessage(scene.first, attr, values);

            return true;
        },
        {'s'});
    setAttributeDescription("sendAllScenes", "Send the given message to all Scenes");

    addAttribute("sendToMasterScene",
        [&](const Values& args) {
            addTask([=]() {
                auto attr = args[0].as<string>();
                Values values = args;
                values.erase(values.begin());
                sendMessage(_masterSceneName, attr, values);
            });

            return true;
        },
        {'s'});
    setAttributeDescription("sendToMasterScene", "Send the given message to the master Scene");

    addAttribute("pingTest",
        [&](const Values& args) {
            auto doPing = args[0].as<int>();
            if (doPing)
            {
                addPeriodicTask("pingTest", [&]() {
                    static auto frameIndex = 0;
                    if (frameIndex == 0)
                    {
                        for (auto& scene : _scenes)
                        {
                            Timer::get() << "pingScene " + scene.first;
                            sendMessage(scene.first, "ping", {});
                        }
                    }
                    frameIndex = (frameIndex + 1) % 60;
                });
            }
            else
            {
                removePeriodicTask("pingTest");
            }

            return true;
        },
        {'n'});
    setAttributeDescription("pingTest", "Activate ping test if set to 1");

    addAttribute("swapTest",
        [&](const Values& args) {
            _swapSynchronizationTesting = args[0].as<int>();
            if (_swapSynchronizationTesting)
            {
                addPeriodicTask("swapTest", [&]() {
                    sendMessage(SPLASH_ALL_PEERS, "swapTest", {1});
                    static auto frameNbr = 0;
                    static auto frameStatus = 0;
                    auto color = glm::vec4(0.0);

                    if (frameNbr == 0 && frameStatus == 0)
                    {
                        color = glm::vec4(0.0, 0.0, 0.0, 1.0);
                        frameStatus = 1;
                    }
                    else if (frameNbr == 0 && frameStatus == 1)
                    {
                        color = glm::vec4(1.0, 1.0, 1.0, 1.0);
                        frameStatus = 0;
                    }

                    if (frameNbr == 0)
                        sendMessage(SPLASH_ALL_PEERS, "swapTestColor", {color[0], color[1], color[2], color[3]});

                    frameNbr = (frameNbr + 1) % _swapSynchronizationTesting;
                });
            }
            else
            {
                removePeriodicTask("swapTest");
                addTask([&]() { sendMessage(SPLASH_ALL_PEERS, "swapTest", {0}); });
            }
            return true;
        },
        {'n'});
    setAttributeDescription("swapTest", "Activate video swap test if set to 1");

    addAttribute("wireframe",
        [&](const Values& args) {
            addTask([=]() { sendMessage(SPLASH_ALL_PEERS, "wireframe", {args[0].as<int>()}); });

            return true;
        },
        {'n'});
    setAttributeDescription("wireframe", "Show all meshes as wireframes if set to 1");

#if HAVE_LINUX
    addAttribute("forceRealtime",
        [&](const Values& args) {
            _enforceRealtime = args[0].as<int>();

            if (!_enforceRealtime)
                return true;

            addTask([=]() {
                if (Utils::setRealTime())
                    Log::get() << Log::MESSAGE << "World::" << __FUNCTION__ << " - Set to realtime priority" << Log::endl;
                else
                    Log::get() << Log::WARNING << "World::" << __FUNCTION__ << " - Unable to set scheduling priority" << Log::endl;
            });

            return true;
        },
        [&]() -> Values { return {(int)_enforceRealtime}; },
        {'n'});
    setAttributeDescription("forceRealtime", "Ask the scheduler to run Splash with realtime priority.");
#endif

    addAttribute("framerate",
        [&](const Values& args) {
            _worldFramerate = std::max(1, args[0].as<int>());
            return true;
        },
        [&]() -> Values { return {(int)_worldFramerate}; },
        {'n'});
    setAttributeDescription("framerate", "Set the minimum refresh rate for the world (adapted to video framerate)");

#if HAVE_PORTAUDIO
    addAttribute("clockDeviceName",
        [&](const Values& args) {
            addTask([=]() {
                auto clockDeviceName = args[0].as<string>();
                if (clockDeviceName != _clockDeviceName)
                {
                    _clockDeviceName = clockDeviceName;
                    _clock.reset();
                    _clock = unique_ptr<LtcClock>(new LtcClock(true, _clockDeviceName));
                }
            });

            return true;
        },
        [&]() -> Values { return {_clockDeviceName}; },
        {'s'});
    setAttributeDescription("clockDeviceName", "Set the audio device name from which to read the LTC clock signal");
#endif

    addAttribute("configurationPath", [&](const Values& /*args*/) { return true; }, [&]() -> Values { return {_configurationPath}; }, {'s'});
    setAttributeDescription("configurationPath", "Path to the configuration files");

    addAttribute("mediaPath",
        [&](const Values& args) {
            auto path = args[0].as<string>();
            if (Utils::isDir(path))
                _mediaPath = args[0].as<string>();
            return true;
        },
        [&]() -> Values { return {_mediaPath}; },
        {'s'});
    setAttributeDescription("mediaPath", "Path to the media files");

    addAttribute("looseClock",
        [&](const Values& args) {
            Timer::get().setLoose(args[0].as<bool>());
            return true;
        },
        [&]() -> Values { return {static_cast<int>(Timer::get().isLoose())}; },
        {'n'});

    addAttribute("clock", [&](const Values& /*args*/) { return true; }, [&]() -> Values { return {Timer::getTime()}; }, {});
    setAttributeDescription("clock", "Current World clock (not settable)");

    addAttribute("masterClock",
        [&](const Values& /*args*/) { return true; },
        [&]() -> Values {
            Timer::Point masterClock;
            if (Timer::get().getMasterClock(masterClock))
                return {masterClock.years, masterClock.months, masterClock.days, masterClock.hours, masterClock.mins, masterClock.secs, masterClock.frame, masterClock.paused};
            else
                return {};
        },
        {});
    setAttributeDescription("masterClock", "Current World master clock (not settable)");

    RootObject::registerAttributes();
}

/*************/
void World::initializeTree()
{
    _tree.setName(_name);
}

} // namespace Splash
