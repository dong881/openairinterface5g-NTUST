#/*
# * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
# * contributor license agreements.  See the NOTICE file distributed with
# * this work for additional information regarding copyright ownership.
# * The OpenAirInterface Software Alliance licenses this file to You under
# * the OAI Public License, Version 1.1  (the "License"); you may not use this file
# * except in compliance with the License.
# * You may obtain a copy of the License at
# *
# *      http://www.openairinterface.org/?page_id=698
# *
# * Unless required by applicable law or agreed to in writing, software
# * distributed under the License is distributed on an "AS IS" BASIS,
# * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# * See the License for the specific language governing permissions and
# * limitations under the License.
# *-------------------------------------------------------------------------------
# * For more information about the OpenAirInterface (OAI) Software Alliance:
# *      contact@openairinterface.org
# */
#---------------------------------------------------------------------
# Python for CI of OAI-eNB + COTS-UE
#
#   Required Python Version
#     Python 3.x
#
#   Required Python Package
#     pexpect
#---------------------------------------------------------------------

#-----------------------------------------------------------
# Import
#-----------------------------------------------------------
import sys	      # arg
import re	       # reg
import logging
import os
import time
import signal

#-----------------------------------------------------------
# OAI Testing modules
#-----------------------------------------------------------
import sshconnection as SSH
import helpreadme as HELP
import constants as CONST
import cls_cluster as OC
import cls_cmd
import cls_module
#-----------------------------------------------------------
# Class Declaration
#-----------------------------------------------------------


class EPCManagement():

	def __init__(self):

		self.IPAddress = ''
		self.UserName = ''
		self.Password = ''
		self.SourceCodePath = ''
		self.Type = ''
		self.PcapFileName = ''
		self.testCase_id = ''
		self.MmeIPAddress = ''
		self.containerPrefix = 'prod'
		self.mmeConfFile = 'mme.conf'
		self.yamlPath = ''
		self.isMagmaUsed = False
		self.cfgDeploy = '--type start-mini --scenario 1 --capture /tmp/oai-cn5g-v1.5.pcap' #from xml, 'mini' is default normal for docker-network.py
		self.cfgUnDeploy = '--type stop-mini --scenario 1' #from xml, 'mini' is default normal for docker-network.py
		self.OCUrl = "https://api.oai.cs.eurecom.fr:6443"
		self.OCRegistry = "default-route-openshift-image-registry.apps.oai.cs.eurecom.fr"
		self.OCUserName = ''
		self.OCPassword = ''
		self.cnID = ''
		self.imageToPull = ''
		self.eNBSourceCodePath = ''

#-----------------------------------------------------------
# EPC management functions
#-----------------------------------------------------------

	def InitializeHSS(self, HTML):
		if self.IPAddress == '' or self.UserName == '' or self.Password == '' or self.SourceCodePath == '' or self.Type == '':
			HELP.GenericHelp(CONST.Version)
			HELP.EPCSrvHelp(self.IPAddress, self.UserName, self.Password, self.SourceCodePath, self.Type)
			sys.exit('Insufficient EPC Parameters')
		mySSH = SSH.SSHConnection()
		mySSH.open(self.IPAddress, self.UserName, self.Password)
		if re.match('OAI-Rel14-Docker', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OAI-Rel14-CUPS', self.Type, re.IGNORECASE):
			raise NotImplemented("core network not supported")
		elif re.match('OAI', self.Type, re.IGNORECASE):
			raise NotImplemented("core network not supported")
		elif re.match('ltebox', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		else:
			logging.error('This option should not occur!')
		mySSH.close()
		HTML.CreateHtmlTestRow(self.Type, 'OK', CONST.ALL_PROCESSES_OK)
		return True

	def InitializeMME(self, HTML):
		if self.IPAddress == '' or self.UserName == '' or self.Password == '' or self.SourceCodePath == '' or self.Type == '':
			HELP.GenericHelp(CONST.Version)
			HELP.EPCSrvHelp(self.IPAddress, self.UserName, self.Password, self.SourceCodePath, self.Type)
			sys.exit('Insufficient EPC Parameters')
		mySSH = SSH.SSHConnection()
		mySSH.open(self.IPAddress, self.UserName, self.Password)
		if re.match('OAI-Rel14-Docker', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OAI-Rel14-CUPS', self.Type, re.IGNORECASE):
			raise NotImplemented("core network not supported")
		elif re.match('OAI', self.Type, re.IGNORECASE):
			raise NotImplemented("core network not supported")
		elif re.match('ltebox', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		else:
			logging.error('This option should not occur!')
		mySSH.close()
		HTML.CreateHtmlTestRow(self.Type, 'OK', CONST.ALL_PROCESSES_OK)
		return True

	def SetMmeIPAddress(self):
		# Not an error if we don't need an EPC
		if self.IPAddress == '' or self.UserName == '' or self.Password == '' or self.SourceCodePath == '' or self.Type == '':
			return
		if self.IPAddress == 'none':
			return
		# Only in case of Docker containers, MME IP address is not the EPC HOST IP address
		if re.match('OAI-Rel14-Docker', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		else:
			self.MmeIPAddress = self.IPAddress

	def InitializeSPGW(self, HTML):
		if self.IPAddress == '' or self.UserName == '' or self.Password == '' or self.SourceCodePath == '' or self.Type == '':
			HELP.GenericHelp(CONST.Version)
			HELP.EPCSrvHelp(self.IPAddress, self.UserName, self.Password, self.SourceCodePath, self.Type)
			sys.exit('Insufficient EPC Parameters')
		mySSH = SSH.SSHConnection()
		mySSH.open(self.IPAddress, self.UserName, self.Password)
		if re.match('OAI-Rel14-Docker', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OAI-Rel14-CUPS', self.Type, re.IGNORECASE):
			raise NotImplemented("core network not supported")
		elif re.match('OAI', self.Type, re.IGNORECASE):
			raise NotImplemented("core network not supported")
		elif re.match('ltebox', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		else:
			logging.error('This option should not occur!')
		mySSH.close()
		HTML.CreateHtmlTestRow(self.Type, 'OK', CONST.ALL_PROCESSES_OK)
		return True

	def Initialize5GCN(self, HTML):
		if self.IPAddress == '' or self.UserName == '' or self.Password == '' or self.Type == '':
			HELP.GenericHelp(CONST.Version)
			HELP.EPCSrvHelp(self.IPAddress, self.UserName, self.Password, self.Type)
			logging.error('Insufficient EPC Parameters')
			return False
		mySSH = cls_cmd.getConnection(self.IPAddress)
		html_cell = ''
		if re.match('ltebox', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OAICN5G', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OC-OAI-CN5G', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		else:
			logging.error('This option should not occur!')
		mySSH.close()
		HTML.CreateHtmlTestRowQueue(self.Type, 'OK', [html_cell])
		return True

	def SetAmfIPAddress(self):
		# Not an error if we don't need an 5GCN
		if self.IPAddress == '' or self.UserName == '' or self.Password == '' or self.SourceCodePath == '' or self.Type == '':
			return
		if self.IPAddress == 'none':
			return
		if re.match('ltebox', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OAICN5G', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OC-OAI-CN5G', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")

	def TerminateHSS(self, HTML):
		mySSH = SSH.SSHConnection()
		mySSH.open(self.IPAddress, self.UserName, self.Password)
		if re.match('OAI-Rel14-Docker', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OAI-Rel14-CUPS', self.Type, re.IGNORECASE):
			raise NotImplemented("core network not supported")
		elif re.match('OAI', self.Type, re.IGNORECASE):
			raise NotImplemented("core network not supported")
		elif re.match('ltebox', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		else:
			logging.error('This should not happen!')
		mySSH.close()
		HTML.CreateHtmlTestRow('N/A', 'OK', CONST.ALL_PROCESSES_OK)
		return True

	def TerminateMME(self, HTML):
		mySSH = SSH.SSHConnection()
		mySSH.open(self.IPAddress, self.UserName, self.Password)
		if re.match('OAI-Rel14-Docker', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OAI', self.Type, re.IGNORECASE) or re.match('OAI-Rel14-CUPS', self.Type, re.IGNORECASE):
			raise NotImplemented("core network not supported")
		elif re.match('ltebox', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		else:
			logging.error('This should not happen!')
		mySSH.close()
		HTML.CreateHtmlTestRow('N/A', 'OK', CONST.ALL_PROCESSES_OK)
		return True

	def TerminateSPGW(self, HTML):
		mySSH = SSH.SSHConnection()
		mySSH.open(self.IPAddress, self.UserName, self.Password)
		if re.match('OAI-Rel14-Docker', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OAI-Rel14-CUPS', self.Type, re.IGNORECASE):
			raise NotImplemented("core network not supported")
		elif re.match('OAI', self.Type, re.IGNORECASE):
			raise NotImplemented("core network not supported")
		elif re.match('ltebox', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		else:
			logging.error('This should not happen!')
		mySSH.close()
		HTML.CreateHtmlTestRow('N/A', 'OK', CONST.ALL_PROCESSES_OK)
		return True

	def Terminate5GCN(self, HTML):
		mySSH = cls_cmd.getConnection(self.IPAddress)
		message = ''
		if re.match('ltebox', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OAICN5G', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OC-OAI-CN5G', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		else:
			logging.error('This should not happen!')
		mySSH.close()
		HTML.CreateHtmlTestRowQueue(self.Type, 'OK', [message])
		return True

	def DeployEpc(self, HTML):
		raise NotImplemented("use cls_corenetwork.py")

	def UndeployEpc(self, HTML):
		raise NotImplemented("use cls_corenetwork.py")

	def LogCollectHSS(self):
		mySSH = SSH.SSHConnection()
		mySSH.open(self.IPAddress, self.UserName, self.Password)
		mySSH.command('cd ' + self.SourceCodePath + '/scripts', '\$', 5)
		mySSH.command('rm -f hss.log.zip', '\$', 5)
		if re.match('OAI-Rel14-Docker', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OAICN5G', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OC-OAI-CN5G', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OAI', self.Type, re.IGNORECASE) or re.match('OAI-Rel14-CUPS', self.Type, re.IGNORECASE):
			raise NotImplemented("core network not supported")
		elif re.match('ltebox', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		else:
			logging.error('This option should not occur!')
		mySSH.close()

	def LogCollectMME(self):
		if self.Type != 'OC-OAI-CN5G':
			mySSH = SSH.SSHConnection()
			mySSH.open(self.IPAddress, self.UserName, self.Password)
			mySSH.command('cd ' + self.SourceCodePath + '/scripts', '\$', 5)
			mySSH.command('rm -f mme.log.zip', '\$', 5)
		else:
			mySSH = cls_cmd.getConnection(self.IPAddress)
		if re.match('OAI-Rel14-Docker', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OAICN5G', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OC-OAI-CN5G', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OAI', self.Type, re.IGNORECASE) or re.match('OAI-Rel14-CUPS', self.Type, re.IGNORECASE):
			raise NotImplemented("core network not supported")
		elif re.match('ltebox', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		else:
			logging.error('This option should not occur!')
		mySSH.close()

	def LogCollectSPGW(self):
		mySSH = SSH.SSHConnection()
		mySSH.open(self.IPAddress, self.UserName, self.Password)
		mySSH.command('cd ' + self.SourceCodePath + '/scripts', '\$', 5)
		mySSH.command('rm -f spgw.log.zip', '\$', 5)
		if re.match('OAI-Rel14-Docker', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OAICN5G', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OC-OAI-CN5G', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		elif re.match('OAI', self.Type, re.IGNORECASE) or re.match('OAI-Rel14-CUPS', self.Type, re.IGNORECASE):
			raise NotImplemented("core network not supported")
		elif re.match('ltebox', self.Type, re.IGNORECASE):
			raise NotImplemented("use cls_corenetwork.py")
		else:
			logging.error('This option should not occur!')
		mySSH.close()
