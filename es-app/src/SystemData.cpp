#include "SystemData.h"
#include "Gamelist.h"
#include <boost/filesystem.hpp>
#include <fstream>
#include <stdlib.h>
#include <SDL_joystick.h>
#include "Renderer.h"
#include "Log.h"
#include "InputManager.h"
#include <iostream>
#include "Settings.h"
#include "FileSorts.h"

std::vector<SystemData*> SystemData::sSystemVector;

namespace fs = boost::filesystem;

SystemData::SystemData(const std::string& name, const std::string& fullName, SystemEnvironmentData* envData, const std::string& themeFolder, bool CollectionSystem) :
	mName(name), mFullName(fullName), mEnvData(envData), mThemeFolder(themeFolder), mIsCollectionSystem(CollectionSystem), mIsGameSystem(true)
{
	mFilterIndex = new FileFilterIndex();

	// if it's an actual system, initialize it, if not, just create the data structure
	if(!CollectionSystem)
	{
		mRootFolder = new FileData(FOLDER, mEnvData->mStartPath, mEnvData, this);
		mRootFolder->metadata.set("name", mFullName);

		if(!Settings::getInstance()->getBool("ParseGamelistOnly"))
			populateFolder(mRootFolder);

		if(!Settings::getInstance()->getBool("IgnoreGamelist"))
			parseGamelist(this);

		mRootFolder->sort(FileSorts::SortTypes.at(0));
	}
	else
	{
		// virtual systems are updated afterwards, we're just creating the data structure
		mRootFolder = new FileData(FOLDER, "" + name, mEnvData, this);
	}
	setIsGameSystemStatus();
	loadTheme();
}

SystemData::~SystemData()
{
	//save changed game data back to xml
	if(!Settings::getInstance()->getBool("IgnoreGamelist") && Settings::getInstance()->getBool("SaveGamelistsOnExit") && !mIsCollectionSystem)
	{
		updateGamelist(this);
	}

	delete mRootFolder;
	delete mFilterIndex;
}

void SystemData::setIsGameSystemStatus()
{
	// we exclude non-game systems from specific operations (i.e. the "RetroPie" system, at least)
	// if/when there are more in the future, maybe this can be a more complex method, with a proper list
	// but for now a simple string comparison is more performant
	mIsGameSystem = (mName != "retropie");
}

#ifndef WIN32
// test to see if a file is hidden in *nix (dot-prefixed)
// could be expanded to check for Windows hidden attribute
bool isHidden(const fs::path &filePath)
{
	fs::path::string_type fileName = filePath.filename().string();
	if(fileName[0] == '.')
	{
		return true;
	}

	return false;
}
#endif

void SystemData::populateFolder(FileData* folder)
{
	const fs::path& folderPath = folder->getPath();
	if(!fs::is_directory(folderPath))
	{
		LOG(LogWarning) << "Error - folder with path \"" << folderPath << "\" is not a directory!";
		return;
	}

	const std::string folderStr = folderPath.generic_string();

	//make sure that this isn't a symlink to a thing we already have
	if(fs::is_symlink(folderPath))
	{
		//if this symlink resolves to somewhere that's at the beginning of our path, it's gonna recurse
		if(folderStr.find(fs::canonical(folderPath).generic_string()) == 0)
		{
			LOG(LogWarning) << "Skipping infinitely recursive symlink \"" << folderPath << "\"";
			return;
		}
	}

	fs::path filePath;
	std::string extension;
	bool isGame;
	bool showHidden = Settings::getInstance()->getBool("ShowHiddenFiles");
	for(fs::directory_iterator end, dir(folderPath); dir != end; ++dir)
	{
		filePath = (*dir).path();

		if(filePath.stem().empty())
			continue;

		//this is a little complicated because we allow a list of extensions to be defined (delimited with a space)
		//we first get the extension of the file itself:
		extension = filePath.extension().string();

		//fyi, folders *can* also match the extension and be added as games - this is mostly just to support higan
		//see issue #75: https://github.com/Aloshi/EmulationStation/issues/75

		isGame = false;
		if(std::find(mEnvData->mSearchExtensions.begin(), mEnvData->mSearchExtensions.end(), extension) != mEnvData->mSearchExtensions.end())
		{
#ifndef WIN32
			// skip hidden files
			if(!showHidden && isHidden(filePath))
				continue;
#endif

			FileData* newGame = new FileData(GAME, filePath.generic_string(), mEnvData, this);
			folder->addChild(newGame);
			isGame = true;
		}

		//add directories that also do not match an extension as folders
		if(!isGame && fs::is_directory(filePath))
		{
			FileData* newFolder = new FileData(FOLDER, filePath.generic_string(), mEnvData, this);
			populateFolder(newFolder);

			//ignore folders that do not contain games
			if(newFolder->getChildrenByFilename().size() == 0)
				delete newFolder;
			else
				folder->addChild(newFolder);
		}
	}
}

std::vector<std::string> readList(const std::string& str, const char* delims = " \t\r\n,")
{
	std::vector<std::string> ret;

	size_t prevOff = str.find_first_not_of(delims, 0);
	size_t off = str.find_first_of(delims, prevOff);
	while(off != std::string::npos || prevOff != std::string::npos)
	{
		ret.push_back(str.substr(prevOff, off - prevOff));

		prevOff = str.find_first_not_of(delims, off);
		off = str.find_first_of(delims, prevOff);
	}

	return ret;
}

//creates systems from information located in a config file
bool SystemData::loadConfig()
{
	deleteSystems();

	std::string path = getConfigPath(false);

	LOG(LogInfo) << "Loading system config file " << path << "...";

	if(!fs::exists(path))
	{
		LOG(LogError) << "es_systems.cfg file does not exist!";
		writeExampleConfig(getConfigPath(true));
		return false;
	}

	pugi::xml_document doc;
	pugi::xml_parse_result res = doc.load_file(path.c_str());

	if(!res)
	{
		LOG(LogError) << "Could not parse es_systems.cfg file!";
		LOG(LogError) << res.description();
		return false;
	}

	//actually read the file
	pugi::xml_node systemList = doc.child("systemList");

	if(!systemList)
	{
		LOG(LogError) << "es_systems.cfg is missing the <systemList> tag!";
		return false;
	}

	for(pugi::xml_node system = systemList.child("system"); system; system = system.next_sibling("system"))
	{
		std::string name, fullname, path, cmd, themeFolder;
		PlatformIds::PlatformId platformId = PlatformIds::PLATFORM_UNKNOWN;

		name = system.child("name").text().get();
		fullname = system.child("fullname").text().get();
		path = system.child("path").text().get();

		// convert extensions list from a string into a vector of strings
		std::vector<std::string> extensions = readList(system.child("extension").text().get());

		cmd = system.child("command").text().get();

		// platform id list
		const char* platformList = system.child("platform").text().get();
		std::vector<std::string> platformStrs = readList(platformList);
		std::vector<PlatformIds::PlatformId> platformIds;
		for(auto it = platformStrs.begin(); it != platformStrs.end(); it++)
		{
			const char* str = it->c_str();
			PlatformIds::PlatformId platformId = PlatformIds::getPlatformId(str);

			if(platformId == PlatformIds::PLATFORM_IGNORE)
			{
				// when platform is ignore, do not allow other platforms
				platformIds.clear();
				platformIds.push_back(platformId);
				break;
			}

			// if there appears to be an actual platform ID supplied but it didn't match the list, warn
			if(str != NULL && str[0] != '\0' && platformId == PlatformIds::PLATFORM_UNKNOWN)
				LOG(LogWarning) << "  Unknown platform for system \"" << name << "\" (platform \"" << str << "\" from list \"" << platformList << "\")";
			else if(platformId != PlatformIds::PLATFORM_UNKNOWN)
				platformIds.push_back(platformId);
		}

		// theme folder
		themeFolder = system.child("theme").text().as_string(name.c_str());

		//validate
		if(name.empty() || path.empty() || extensions.empty() || cmd.empty())
		{
			LOG(LogError) << "System \"" << name << "\" is missing name, path, extension, or command!";
			continue;
		}

		//convert path to generic directory seperators
		boost::filesystem::path genericPath(path);
		path = genericPath.generic_string();

		//expand home symbol if the startpath contains ~
		if(path[0] == '~')
		{
			path.erase(0, 1);
			path.insert(0, getHomePath());
		}

		//create the system runtime environment data
		SystemEnvironmentData* envData = new SystemEnvironmentData;
		envData->mStartPath = path;
		envData->mSearchExtensions = extensions;
		envData->mLaunchCommand = cmd;
		envData->mPlatformIds = platformIds;

		SystemData* newSys = new SystemData(name, fullname, envData, themeFolder);
		if(newSys->getRootFolder()->getChildrenByFilename().size() == 0)
		{
			LOG(LogWarning) << "System \"" << name << "\" has no games! Ignoring it.";
			delete newSys;
		}else{
			sSystemVector.push_back(newSys);
		}
	}
	CollectionSystemManager::get()->loadCollectionSystems();

	return true;
}

void SystemData::writeExampleConfig(const std::string& path)
{
	std::ofstream file(path.c_str());

	file << "<!-- This is the EmulationStation Systems configuration file.\n"
			"All systems must be contained within the <systemList> tag.-->\n"
			"\n"
			"<systemList>\n"
			"	<!-- Here's an example system to get you started. -->\n"
			"	<system>\n"
			"\n"
			"		<!-- A short name, used internally. Traditionally lower-case. -->\n"
			"		<name>nes</name>\n"
			"\n"
			"		<!-- A \"pretty\" name, displayed in menus and such. -->\n"
			"		<fullname>Nintendo Entertainment System</fullname>\n"
			"\n"
			"		<!-- The path to start searching for ROMs in. '~' will be expanded to $HOME on Linux or %HOMEPATH% on Windows. -->\n"
			"		<path>~/roms/nes</path>\n"
			"\n"
			"		<!-- A list of extensions to search for, delimited by any of the whitespace characters (\", \\r\\n\\t\").\n"
			"		You MUST include the period at the start of the extension! It's also case sensitive. -->\n"
			"		<extension>.nes .NES</extension>\n"
			"\n"
			"		<!-- The shell command executed when a game is selected. A few special tags are replaced if found in a command:\n"
			"		%ROM% is replaced by a bash-special-character-escaped absolute path to the ROM.\n"
			"		%BASENAME% is replaced by the \"base\" name of the ROM.  For example, \"/foo/bar.rom\" would have a basename of \"bar\". Useful for MAME.\n"
			"		%ROM_RAW% is the raw, unescaped path to the ROM. -->\n"
			"		<command>retroarch -L ~/cores/libretro-fceumm.so %ROM%</command>\n"
			"\n"
			"		<!-- The platform to use when scraping. You can see the full list of accepted platforms in src/PlatformIds.cpp.\n"
			"		It's case sensitive, but everything is lowercase. This tag is optional.\n"
			"		You can use multiple platforms too, delimited with any of the whitespace characters (\", \\r\\n\\t\"), eg: \"genesis, megadrive\" -->\n"
			"		<platform>nes</platform>\n"
			"\n"
			"		<!-- The theme to load from the current theme set.  See THEMES.md for more information.\n"
			"		This tag is optional. If not set, it will default to the value of <name>. -->\n"
			"		<theme>nes</theme>\n"
			"	</system>\n"
			"</systemList>\n";

	file.close();

	LOG(LogError) << "Example config written!  Go read it at \"" << path << "\"!";
}

void SystemData::deleteSystems()
{
	for(unsigned int i = 0; i < sSystemVector.size(); i++)
	{
		delete sSystemVector.at(i);
	}
	sSystemVector.clear();
}

std::string SystemData::getConfigPath(bool forWrite)
{
	fs::path path = getHomePath() + "/.emulationstation/es_systems.cfg";
	if(forWrite || fs::exists(path))
		return path.generic_string();

	return "/etc/emulationstation/es_systems.cfg";
}

std::string SystemData::getGamelistPath(bool forWrite) const
{
	fs::path filePath;

	filePath = mRootFolder->getPath() / "gamelist.xml";
	if(fs::exists(filePath))
		return filePath.generic_string();

	filePath = getHomePath() + "/.emulationstation/gamelists/" + mName + "/gamelist.xml";
	if(forWrite) // make sure the directory exists if we're going to write to it, or crashes will happen
		fs::create_directories(filePath.parent_path());
	if(forWrite || fs::exists(filePath))
		return filePath.generic_string();

	return "/etc/emulationstation/gamelists/" + mName + "/gamelist.xml";
}

std::string SystemData::getThemePath() const
{
	// where we check for themes, in order:
	// 1. [SYSTEM_PATH]/theme.xml
	// 2. system theme from currently selected theme set [CURRENT_THEME_PATH]/[SYSTEM]/theme.xml
	// 3. default system theme from currently selected theme set [CURRENT_THEME_PATH]/theme.xml

	// first, check game folder
	fs::path localThemePath = mRootFolder->getPath() / "theme.xml";
	if(fs::exists(localThemePath))
		return localThemePath.generic_string();

	// not in game folder, try system theme in theme sets
	localThemePath = ThemeData::getThemeFromCurrentSet(mThemeFolder);

	if (fs::exists(localThemePath))
		return localThemePath.generic_string();

	// not system theme, try default system theme in theme set
	localThemePath = localThemePath.parent_path().parent_path() / "theme.xml";

	return localThemePath.generic_string();
}

bool SystemData::hasGamelist() const
{
	return (fs::exists(getGamelistPath(false)));
}

unsigned int SystemData::getGameCount() const
{
	return mRootFolder->getFilesRecursive(GAME).size();
}

SystemData* SystemData::getRandomSystem()
{
	//  this is a bit brute force. It might be more efficient to just to a while (!gameSystem) do random again...
	unsigned int total = 0;
	for(auto it = sSystemVector.begin(); it != sSystemVector.end(); it++)
	{
		if ((*it)->isGameSystem())
			total ++;
	}

	// get random number in range
	int target = std::round(((double)std::rand() / (double)RAND_MAX) * (total - 1));
	for (auto it = sSystemVector.begin(); it != sSystemVector.end(); it++)
	{
		if ((*it)->isGameSystem())
		{
			if (target > 0)
			{
				target--;
			}
			else
			{
				return (*it);
			}
		}
	}
}

FileData* SystemData::getRandomGame()
{
	std::vector<FileData*> list = mRootFolder->getFilesRecursive(GAME, true);
	unsigned int total = list.size();
	int target = 0;
	// get random number in range
	if (total == 0)
		return NULL;
	target = std::round(((double)std::rand() / (double)RAND_MAX) * (total - 1));
	return list.at(target);
}

unsigned int SystemData::getDisplayedGameCount() const
{
	return mRootFolder->getFilesRecursive(GAME, true).size();
}

void SystemData::loadTheme()
{
	mTheme = std::make_shared<ThemeData>();

	std::string path = getThemePath();

	if(!fs::exists(path)) // no theme available for this platform
		return;

	try
	{
		// build map with system variables for theme to use,
		std::map<std::string, std::string> sysData;
		sysData.insert(std::pair<std::string, std::string>("system.name", getName()));
		sysData.insert(std::pair<std::string, std::string>("system.theme", getThemeFolder()));
		sysData.insert(std::pair<std::string, std::string>("system.fullName", getFullName()));
		
		mTheme->loadFile(sysData, path);
	} catch(ThemeException& e)
	{
		LOG(LogError) << e.what();
		mTheme = std::make_shared<ThemeData>(); // reset to empty
	}
}
