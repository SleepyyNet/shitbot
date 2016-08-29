#include <iostream>
#include "asio.hpp"
#include <chrono>
#include <thread>
#include <map>
#include <mutex>
#include <ctime>
#include <set>
#include <fstream>
#include <random>
#include <iomanip>
#include <locale>
#include <condition_variable>

struct command
{
	std::string who;
	std::string action;
	std::string data;
};

std::string timenow()
{
	std::time_t result = std::time(nullptr);
	std::stringstream ss;
	ss.imbue(std::locale("cs_CZ.utf8"));
	ss << std::put_time(std::localtime(&result), "%c");
	return ss.str();
}

class IrcConnection
{
public:
	IrcConnection();
	~IrcConnection();
	asio::io_service m_io_service;
	std::map<std::string, std::shared_ptr<asio::ip::tcp::socket>> channelSockets;
	void stop();
	void start(std::string pass, std::string nick);
	std::thread work;
	std::thread msg;
	std::map<std::string, std::chrono::high_resolution_clock::time_point> channelTimes;
	void joinChannel(std::string);
	void leaveChannel(std::string);
	bool sendMsg(std::string channel, std::string message);
	bool isAdmin(std::string user);
	void addAdmin(std::string admin);
	void addCmd(std::string cmd, std::string msg);
	std::map<std::string, command> commands;
	int dice()
	{
		return this->dist(this->mt);
	}
	void listenAndHandle(std::string channel);
	std::map<std::string, bool> channelBools;
	std::map<std::string, unsigned> channelMsgs;
	std::mutex irc_m;
	bool quit();
	bool quit_m = false;
	std::vector<std::thread> listenThreads;
	void waitEnd()
	{
		std::unique_lock<std::mutex> lock(irc_m);
		this->quit_cv.wait(lock, [this](){return this->quit_m;});
		lock.unlock();
		//std::cout << "ending\n";
		for(auto& th : this->listenThreads) 
		{
			//std::cout << "nativ: " << th.native_handle() << std::endl;
			if(th.joinable())
				th.join();
		}
		if(this->work.joinable())
			this->work.join();
		if(this->msg.joinable())
			this->msg.join();
		//std::cout << "ended waiting\n";
	}
private:
	std::unique_ptr<asio::io_service::work> wrk;
	void run();
	void msgCount();
	
	std::fstream commandsFile;
	std::fstream adminsFile;
	std::set<std::string> admins;
	std::random_device rd;
	std::mt19937 mt{this->rd()};
	std::uniform_int_distribution<int> dist{0, 100};
	std::string pass;
	std::string nick;
	asio::ip::tcp::resolver::iterator twitch_it;
	std::condition_variable quit_cv;
};

IrcConnection::IrcConnection()
{
	this->adminsFile.open("admins.txt", std::ios::in);
	if(this->adminsFile.is_open() == false)
	{
		this->adminsFile.open("admins.txt", std::ios::out);
		this->adminsFile << "hemirt" << std::endl;
		this->adminsFile.close();
		this->adminsFile.open("admins.txt", std::ios::in);
	}
	{
		std::string line;
		while(std::getline(this->adminsFile, line))
		{
			this->admins.insert(line);
		}
		this->adminsFile.close();
		this->adminsFile.open("admins.txt", std::ios::out | std::ios::app);
	}
	
	this->commandsFile.open("commands.txt", std::ios::in);
	if(this->commandsFile.is_open() == false)
	{
		this->commandsFile.open("commands.txt", std::ios::out);
		this->commandsFile << "!addcmd admins Done" << std::endl;
		this->commandsFile.close();
		this->commandsFile.open("commands.txt", std::ios::in);
	}
	{
		std::string line;
		while(std::getline(this->commandsFile, line))
		{
			std::string delimiter = "#";
			std::vector<std::string> vek;
			size_t pos = 0;
			int i = 1;
			while((pos = line.find(delimiter)) != std::string::npos && i < 4)
			{
				vek.push_back(line.substr(0, pos));
				line.erase(0, pos + delimiter.length());
				i++;
			}
			vek.push_back(line);
			if(vek.size() == 4)
			{
				command cmd = {vek[1], vek[2], vek[3]};
				this->commands.insert(std::pair<std::string, command>(vek[0], cmd));
			}
		}
		this->commandsFile.close();
		this->commandsFile.open("commands.txt", std::ios::out | std::ios::app);
	}

	std::cout << "lulopened\n";
}
IrcConnection::~IrcConnection()
{
	this->adminsFile.close();
	this->commandsFile.close();
}

bool IrcConnection::isAdmin(std::string user)
{
	if(this->admins.count(user) == 1)
		return true;
	else return false;
}

void IrcConnection::addAdmin(std::string admin)
{
	if(this->admins.count(admin) == 0)
	{
		this->admins.insert(admin);
		this->adminsFile.seekp(0, std::ios::end);
		this->adminsFile << admin << std::endl;
	}
}

void IrcConnection::addCmd(std::string cmd, std::string message)
{
	if(this->commands.count(cmd) == 0)
	{
		command cmds;
		
		std::string delimiter = "#";
		std::vector<std::string> vek;
		size_t pos = 0;
		int i = 1;
		while((pos = message.find(delimiter)) != std::string::npos && i < 3)
		{
			vek.push_back(message.substr(0, pos));
			message.erase(0, pos + delimiter.length());
			i++;
		}
		vek.push_back(message);
		if(vek.size() == 3)
		{
			cmds = {vek[0], vek[1], vek[2]};
			this->commands.insert(std::pair<std::string, command>(cmd, cmds));
			this->commandsFile.seekp(0, std::ios::end);
			this->commandsFile << cmd << "#" << cmds.who << "#" << cmds.action << "#" << cmds.data << std::endl;
		}
		

	}
}

void IrcConnection::joinChannel(std::string chn)
{
	std::shared_ptr<asio::ip::tcp::socket> sock(new asio::ip::tcp::socket(this->m_io_service));
	this->channelSockets.insert(std::pair<std::string, std::shared_ptr<asio::ip::tcp::socket>>(chn, sock));
	asio::connect(*sock, this->twitch_it);
	
	std::string passx = "PASS " + this->pass + "\r\n";
	std::string nickx = "NICK " + this->nick + "\r\n";
	std::string cmds = "CAP REQ :twitch.tv/commands\r\n";
	
	sock->send(asio::buffer(passx));
	sock->send(asio::buffer(nickx));
	sock->send(asio::buffer(cmds));	
	
	std::string join = "JOIN #" + chn + " \r\n";
	sock->send(asio::buffer(join));
	this->channelTimes.insert(std::pair<std::string, std::chrono::high_resolution_clock::time_point>(chn, std::chrono::high_resolution_clock::now()));
	this->channelMsgs.insert(std::pair<std::string, unsigned>(chn, 0));
	this->channelBools.insert(std::pair<std::string, bool>(chn, false));
	std::cout << "joined" << chn << std::endl;	
	this->listenThreads.push_back(std::thread(&IrcConnection::listenAndHandle, this, chn));
	std::cout << "started thread: " << chn << std::endl;
	
}

void IrcConnection::leaveChannel(std::string chn)
{
	std::string part = "PART #" + chn + " \r\n";
	this->channelSockets[chn]->send(asio::buffer(part));
	this->channelSockets[chn]->close();
	this->channelSockets.erase(chn);
	this->channelBools.erase(chn);
	this->channelMsgs.erase(chn);
	this->channelTimes.erase(chn);
}

bool IrcConnection::quit()
{
	std::lock_guard<std::mutex> lck(irc_m);
	return this->quit_m;
}

void IrcConnection::stop()
{
	{
		std::lock_guard<std::mutex> lck(irc_m);
		this->wrk.reset();
		this->quit_m = true;
		for(auto &i : this->channelSockets)
		{
			this->leaveChannel(i.first);
			i.second->close();
			std::cout << "left: " << i.first << std::endl;
		}
		this->channelSockets.clear();
		this->channelBools.clear();
		this->channelMsgs.clear();
		this->channelTimes.clear();	
	}
	this->quit_cv.notify_all();
}

void IrcConnection::start(std::string pass, std::string nick)
{
	std::lock_guard<std::mutex> lck(irc_m);
	this->quit_m = false;
	this->pass = pass;
	this->nick = nick;
	
	asio::ip::tcp::resolver resolver(m_io_service);
	asio::ip::tcp::resolver::query query("irc.chat.twitch.tv", "6667");
	this->twitch_it = resolver.resolve(query);
	
	this->work = std::thread(&IrcConnection::run, this);
	this->msg = std::thread(&IrcConnection::msgCount, this);
	//this->pingThread = std::thread(&IrcConnection::pingAll, this);
}

void handler(const asio::error_code& error,std::size_t bytes_transferred)
{
	std::cout << "sent msg: " << bytes_transferred << std::endl;
}

void IrcConnection::run()
{
	this->wrk = std::make_unique<asio::io_service::work>(this->m_io_service);
	this->m_io_service.run();
	this->m_io_service.reset();
	std::cout << "end wrk\n";
}

void IrcConnection::msgCount()
{
	while(!(this->quit()))
	{
		std::this_thread::sleep_for(std::chrono::seconds(2));
		for(auto & i : this->channelMsgs)
		{
			if(i.second > 0)
			{
				i.second--;
			}
		}
	}
	std::cout << "end msg\n";
}

bool IrcConnection::sendMsg(std::string channel, std::string msg)
{
	if(this->channelMsgs[channel] < 19 && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - this->channelTimes[channel]).count() > 1500)
	{
		std::cout << "Sending msg okay\n";
		std::string sendirc = "PRIVMSG #" + channel + " :" + msg; // + " \r\n";
		if(sendirc.length() > 387)
			sendirc = sendirc.substr(0, 387) + " \r\n";
		else sendirc += " \r\n";
		std::cout << "sendirc: " << sendirc << std::endl;
		this->channelSockets[channel]->async_send(asio::buffer(sendirc), handler);
		this->channelMsgs[channel]++;
		this->channelTimes[channel] = std::chrono::high_resolution_clock::now();
		return true;
	}
	else 
	{
		std::cout << "sending msg not okay\n";
		return false;
	}
}

void handleCommands(IrcConnection* myIrc, std::string user, std::string channel, std::string msg)
{
	//std::cout << "HANDLING COMMAND\n";
	if(msg.compare(0, strlen("!quit"), "!quit") == 0 && user == "hemirt")
	{
		myIrc->stop();
		return;
	}
	
	if(msg.compare(0, strlen("!rcn"), "!rcn") == 0 && user == "hemirt")
	{
		myIrc->leaveChannel(channel);
		myIrc->joinChannel(channel);
		return;
	}
	
	
	
	
	if(msg.compare(0, strlen("!addcmd"), "!addcmd") == 0 && user == "hemirt")
	{
		std::string delimiter = "#";
		std::vector<std::string> vek;
		size_t pos = 0;
		int i = 1;
		while((pos = msg.find(delimiter)) != std::string::npos && i < 5)
		{
			vek.push_back(msg.substr(0, pos));
			msg.erase(0, pos + delimiter.length());
			i++;
		}
		vek.push_back(msg);
		
		for(auto i : vek)
		{
			std::cout << "VKX:" << i << std::endl;
		}
		
		if(vek.size() == 5)
		{
			std::string cmd = vek[2] + "#" + vek[3] + "#" + vek[4];
			std::cout << "adding: " << vek[1] << "#" << cmd << std::endl;
			myIrc->addCmd(vek[1], cmd);
		}
		return;
	}
	
	if(msg.compare(0, strlen("!rolet "), "!rolet ") == 0)
	{
		std::vector<std::string> vek;
		while(msg.find(".") != std::string::npos)
		{
			msg.replace(msg.find("."), 1, " ");
		}
		size_t pos = 0;
		int i = 0;
		while((pos = msg.find(" ")) != std::string::npos && i < 2)
		{
			vek.push_back(msg.substr(0, pos));
			msg.erase(0, pos + 1);
			i++;
		}
		vek.push_back(msg);
		if(vek.size() >= 2)
		{
			std::string msgback;
			if(vek.at(1).find("!untuck") != std::string::npos)
			{
				msgback = user + " one does not simply untuck LUL";
			}
			else
			{
				while(true)
				{
					int rolet = myIrc->dice();
					if(rolet > 50)
					{
						msgback = user + " won " + vek.at(1) + " in rolet and is now rich as fuck! FeelsAmazingMan";
						break;
					}
					else if(rolet < 50)
					{
						msgback = user + " lost " + vek.at(1) + " in rolet and now has nothing left! FeelsBadMan";
						break;
					}
					else continue;
				}
			}
			myIrc->sendMsg(channel, msgback);
			return;
				
		}
		return;
		
	}

	while(msg.find(".") != std::string::npos)
	{
		msg.replace(msg.find("."), 1, " ");
	}
	msg.erase(0, 1);
	while(msg.find("!") != std::string::npos && user != "hemirt")
	{
		msg.replace(msg.find("!"), 1, " ");
	}
	std::string msgcopy = msg;
	std::string delimiter = " ";
	std::vector<std::string> vekmsg;
	size_t pos = 0;
	while((pos = msgcopy.find(delimiter)) != std::string::npos)
	{
		vekmsg.push_back(msgcopy.substr(0, pos));
		msgcopy.erase(0, pos + delimiter.length());
	}
	vekmsg.push_back(msgcopy);
	
	for(auto it = myIrc->commands.crbegin(); it != myIrc->commands.crend(); ++it)
	{
		if(vekmsg.at(0).compare(0, vekmsg.at(0).size(), it->first, 1, it->first.size()) == 0)
		{

			std::string msgback = it->second.data;
			while(msgback.find("@me@") != std::string::npos)
			{
				msgback.replace(msgback.find("@me@"), 4, user);
			}
			int i = 1;
			std::stringstream ss;
			ss << "@id" << i << "@";
			while(msgback.find(ss.str()) != std::string::npos && i < vekmsg.size())
			{
				msgback.replace(msgback.find(ss.str()), ss.str().size(), vekmsg[i]);
				i++;
				ss.str("");
				ss << "@id" << i << "@";
			}
			std::string all = msg.substr(it->first.size() - 1, std::string::npos);
			while(msgback.find("@all@") != std::string::npos)
			{
				msgback.replace(msgback.find("@all@"), 5, all);
			}
			
			int rnd = myIrc->dice();
			while(msgback.find("@irnd@") != std::string::npos)
			{
				msgback.replace(msgback.find("@irnd@"), 6, std::to_string(rnd));
			}
			
			size_t pos = 0;
			while((pos = msgback.find("@ifrnd@")) != std::string::npos)
			{
				msgback.erase(pos, 7);
				std::cout << "msgback: " << "\"" << msgback << "\"" << std::endl;
				std::cout << "pos: " << msgback.at(pos) << std::endl;
				if(msgback.at(pos) == '<')
				{
					msgback.erase(pos, 1);
					size_t hash = msgback.find('#', pos);
					std::string number = msgback.substr(pos, hash-pos);
					int num = std::atoi(number.c_str());
					std::cout << "substr: " << msgback.substr(pos, hash-pos) << std::endl;
					msgback.erase(pos, hash-pos);
					if(rnd < num)
					{
						msgback.erase(pos, 1);
						size_t end = msgback.find('#', pos);
						msgback.erase(end, 1);
					}
					else
					{
						msgback.erase(pos, 1);
						size_t end = msgback.find('#', pos);
						msgback.erase(pos, end-pos + 1);
					}
				}
				else if(msgback.at(pos) == '>')
				{
					msgback.erase(pos, 1);
					size_t hash = msgback.find('#', pos);
					std::string number = msgback.substr(pos, hash-pos);
					int num = std::atoi(number.c_str());
					std::cout << "substr: " << msgback.substr(pos, hash-pos) << std::endl;
					msgback.erase(pos, hash-pos);
					if(rnd > num)
					{
						msgback.erase(pos, 1);
						size_t end = msgback.find('#', pos);
						msgback.erase(end, 1);
					}
					else
					{
						msgback.erase(pos, 1);
						size_t end = msgback.find('#', pos);
						msgback.erase(pos, end-pos + 1);
					}
				}
				if(msgback.at(pos) == '=')
				{
					msgback.erase(pos, 1);
					size_t hash = msgback.find('#', pos);
					std::string number = msgback.substr(pos, hash-pos);
					int num = std::atoi(number.c_str());
					std::cout << "substr: " << msgback.substr(pos, hash-pos) << std::endl;
					msgback.erase(pos, hash-pos);
					if(rnd == num)
					{
						msgback.erase(pos, 1);
						size_t end = msgback.find('#', pos);
						msgback.erase(end, 1);
					}
					else
					{
						msgback.erase(pos, 1);
						size_t end = msgback.find('#', pos);
						msgback.erase(pos, end-pos + 1);
					}
				}
			}
			
			while(msgback.find("@time@") != std::string::npos)
			{
				msgback.replace(msgback.find("@time@"), 6, timenow());
			}
			
			if(it->second.who == "all")
			{
				if(it->second.action == "say")
				{
					myIrc->sendMsg(channel, msgback);
					return;
				}
			}
			else if(it->second.who == "admin")
			{
				if(myIrc->isAdmin(user))
				{
					if(it->second.action == "say")
					{
						myIrc->sendMsg(channel, msgback);
						return;
					}
					else if (it->second.action == "repeat")
					{
						std::cout << "msgb: " << msgback << std::endl;
						std::string delimiter = " ";
						msgback.erase(0, 1);
						size_t pos = 0;
						int rp = 0;
						if((pos = msgback.find(delimiter)) != std::string::npos)
						{
							rp = std::atoi(msgback.substr(0, pos).c_str());
							msgback.erase(0, pos + delimiter.length());
						}
						for(int y = 0; y < rp; y++)
						{
							myIrc->sendMsg(channel, msgback);
							std::this_thread::sleep_for(std::chrono::milliseconds(1501));
						}
					}
				}
			}
			else if(it->second.who == user)
			{
				if(it->second.action == "say")
				{
					myIrc->sendMsg(channel, msgback);
					return;
				}
			}
		}
	}
}

void IrcConnection::listenAndHandle(std::string chn)
{
	try
		{
			while(!(this->quit()))
			{
				std::vector<char> buf(4096);
				this->channelSockets[chn]->read_some(asio::buffer(buf));
				std::string line(buf.begin(), buf.end());
				
				std::string delimiter = "\r\n";
				std::vector<std::string> vek;
				size_t pos = 0;
				while((pos = line.find(delimiter)) != std::string::npos)
				{
					vek.push_back(line.substr(0, pos));
					line.erase(0, pos + delimiter.length());
				}
				//std::cout << "VEKBEGIN\n";
				for(int i = 0; i < vek.size(); ++i)
				{
					std::string oneline = vek.at(i);
				
					//std::cout << "oneline:\n" << oneline << "\nendline" <<std::endl;
					if(oneline.find("PRIVMSG") != std::string::npos)
					{
						size_t pos = oneline.find("PRIVMSG #") + strlen("PRIVMSG #");		
						std::string channel = oneline.substr(pos, oneline.find(":", oneline.find("PRIVMSG #")) - pos - 1);
						std::string ht_chn = "#" + channel;
						std::string msg = oneline.substr(oneline.find(":", oneline.find(ht_chn)) + 1, std::string::npos);
						std::string user = oneline.substr(oneline.find(":") + 1, oneline.find("!") - oneline.find(":") - 1);

						if(msg[0] == '!')
						{
							auto exx = std::thread(handleCommands, this, user, channel, msg);
							exx.detach();
						}
					}
					else if(oneline.find("PING") != std::string::npos)
					{
						std::cout << "PONGING\n";
						std::string pong = "PONG :tmi.twitch.tv\r\n";
						this->channelSockets[chn]->async_send(asio::buffer(pong), handler);
					}
				}
				//std::cout<< "VEKEND\n";

			}
		}
	catch (std::exception& e)
	{
		std::cout << "exc : " << chn << " " << e.what() << std::endl;
	}
}

int main(int argc, char *argv[])
{
	IrcConnection myIrc;
	myIrc.start(argv[1], argv[2]);

	myIrc.joinChannel("pajlada");
	myIrc.joinChannel("hemirt");
	myIrc.joinChannel("forsenlol");

	myIrc.waitEnd();
	return 0;
}
